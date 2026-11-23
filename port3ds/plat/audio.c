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
#include <stdint.h>
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

static void wav_write(const char* name, const int16_t* data, uint32_t frames, uint32_t rate) {
    /* The file is built in memory and queued to the SD service (core 1's worker): no SD call here. */
    uint8_t* file = malloc(44 + (size_t)frames * 4);
    if (file != NULL) {
        uint32_t bytes = frames * 4;
        uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0};
        uint32_t riff = 36 + bytes, byte_rate = rate * 4;
        memcpy(h + 4, &riff, 4);
        memcpy(h + 24, &rate, 4);
        memcpy(h + 28, &byte_rate, 4);
        h[32] = 4;
        h[34] = 16;
        memcpy(h + 36, "data", 4);
        memcpy(h + 40, &bytes, 4);
        memcpy(file, h, sizeof(h));
        memcpy(file + sizeof(h), data, bytes);
        plat_sd_write_file_own(name, file, sizeof(h) + bytes, 0);
        PLOG_I("audio: wrote %s (%lu frames)", name, (unsigned long)frames);
    }
}

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
    wav_write("audio.wav", s_wav, s_wav_frames, s_rate);
    free(s_wav);
    s_wav = NULL;
}

/* A channel's mix: front left and right only. */
static void set_mix(int ch, float l, float r) {
    float mix[12];
    memset(mix, 0, sizeof(mix));
    mix[0] = l;
    mix[1] = r;
    ndspChnSetMix(ch, mix);
}

static uint32_t s_fill_pos; /* output frame of the quantum being filled (voice event times) */
static uint64_t s_busy_ticks; /* system ticks spent in the fill calls and in applying voice events */

static void fill_block(int16_t* out) {
    /* quantum by quantum, so each quantum's voice calls get its own time */
    for (uint32_t q = 0; q < s_buf_frames; q += s_quantum) {
        s_fill_pos = s_frames + q;
        uint64_t t0 = svcGetSystemTick();
        s_fill(out + q * 2, s_quantum);
        s_busy_ticks += svcGetSystemTick() - t0;
    }
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
        /* busy: the share of the time spent filling (the software mix, MusyX's frame, the voices' set-up) */
        static uint64_t busy_before, tick_before;
        uint64_t now = svcGetSystemTick();
        uint32_t busy = tick_before ? (uint32_t)((s_busy_ticks - busy_before) * 1000 / (now - tick_before)) : 0;
        PLOG_I("audio: peak %d of 32767 over the last 10 s, %lu underruns, fill busy %lu.%lu%%", peak,
               (unsigned long)s_underruns, (unsigned long)(busy / 10), (unsigned long)(busy % 10));
        busy_before = s_busy_ticks;
        tick_before = now;
        peak = 0;
        since = s_frames;
    }
}

/* ------------------------------------------------------------------------
 * NDSP voices (switch ndspvoices=1; see plat/audio.h and docs/PORT_3DS_AUDIO.md)
 *
 * Channel 0 plays the software mix. Channels 1..23 play one sound voice each:
 * the DSP decodes its DSP-ADPCM sample from a copy in linear memory (the DSP
 * cannot read the heap, where ARAM is), resamples it at the voice's rate and
 * mixes it at its gains. The game side (game/audio.c) calls start, mix and
 * stop from inside fill; each call becomes an event stamped with the output
 * frame its quantum's sound reaches the speakers at (s_fill_pos plus the
 * lead), and the thread applies the events as channel 0's playback reaches
 * that frame, once per DSP frame. So the voices keep time with the software
 * mix to about one DSP frame (5 ms).
 *
 * Samples are cached by ARAM address. A cached copy is checked against ARAM
 * (memcmp) at every start, because the game reloads sound groups at the same
 * addresses. NDSP loops whole wave buffers, and an ADPCM buffer must start
 * on a 14-sample frame; most TTYD loops start inside a frame, so the copy
 * gets a re-aligned loop: the loop start moves forward to the next frame
 * boundary, the end moves by the same amount, and the one or two frames
 * around the old end are re-encoded with the sample's own coefficients so
 * that the waveform carries on into the loop (realign_loop).
 */

#define VOICE_CHANNELS 23  /* NDSP has 24; channel 0 is the software mix */
#define VOICE_EVENTS 1024
#define CACHE_ENTRIES 160

enum { EV_START = 1, EV_MIX, EV_STOP };

typedef struct CacheEntry {
    uint32_t key;          /* ARAM address of the first frame; 0: free */
    const uint8_t* src;    /* where the copy came from */
    uint32_t src_bytes;    /* bytes compared with src at each start */
    uint8_t* lin;          /* the copy (linear memory) */
    uint32_t lin_bytes;
    uint32_t end, loop_start; /* as played: loop re-aligned, end moved with it */
    uint8_t looping, realigned;
    uint16_t loop_ps;
    int16_t loop_yn1, loop_yn2;
    uint16_t users;        /* channels playing it, and start events queued */
    uint32_t last_use;     /* LRU */
    uint32_t want_end, want_loop; /* the request's end and loop start (part of the identity) */
} CacheEntry;

typedef struct VoiceEvent {
    uint32_t at;   /* output frame */
    uint8_t type, ch;
    int16_t cache; /* EV_START: the cache entry */
    uint32_t start; /* EV_START: the first sample */
    PlatVoiceMix mix;
    uint16_t coefs[16];
    uint16_t ps;
    int16_t yn1, yn2;
} VoiceEvent;

typedef struct VoiceChannel {
    int16_t cache;           /* entry playing, -1 none */
    ndspWaveBuf wb[2][2];    /* two sets, alternated between starts */
    ndspAdpcmData ctx[2][2];
    uint8_t set;
    uint8_t lpf_on;
    int16_t lpf_a0, lpf_b0;
    uint8_t interp;
    int8_t stream;           /* stream slot playing, -1 none */
    int8_t str_latest;       /* the newest stream slot started on it (refills are for that one) */
} VoiceChannel;

/* A stream on a channel: the ring mirrored in linear memory, played as a chain
 * of short wave buffers (STREAM_SEG samples, STREAM_DEPTH queued). The ring is
 * refilled by the game behind MusyX's position, which runs ahead of what is
 * heard by the lead, so the mirror takes each refill later, when the output
 * has passed it (EV_SDATA, its own queue so it holds nothing else back); a
 * segment is queued only while it ends within the part of the ring the mirror
 * still holds for certain. */
#define STREAM_SLOTS 8
#define STREAM_SEGS 4
#define STREAM_DEPTH 3
#define STREAM_SEG 560 /* samples; a multiple of 14 */
#define STREAM_EVENTS 256

typedef struct StreamSlot {
    uint8_t used, pcm16, first;
    uint8_t ch;
    uint16_t gen, users;
    const uint8_t* ring;
    uint8_t* mirror;
    uint32_t bytes, samples;
    uint32_t abs_next;    /* absolute sample index of the next segment */
    uint32_t free_wake;   /* closing: freed at this wake */
    ndspWaveBuf wb[STREAM_SEGS];
    uint32_t wb_abs[STREAM_SEGS];
    ndspAdpcmData ctx;
    uint16_t ps;
} StreamSlot;

typedef struct StreamEvent {
    uint32_t at, off, len;
    uint8_t slot;
    uint16_t gen;
} StreamEvent;

static int s_voices_on;
static uint32_t s_voice_lead;
static VoiceEvent s_ev[VOICE_EVENTS];
static uint32_t s_ev_head, s_ev_tail; /* produced and consumed on the audio thread */
static VoiceChannel s_vch[VOICE_CHANNELS + 1];
static CacheEntry s_cache[CACHE_ENTRIES];
static uint32_t s_cache_bytes, s_cache_budget, s_cache_clock;
static uint32_t s_wbuf_pos[WAVEBUFS]; /* output frame of each channel-0 buffer's first sample */
static uint32_t s_play_pos;
static struct {
    uint32_t starts, refused, hits, misses, realigned, evicted, late, dropped_events;
    uint32_t sw, ndsp, aux, overflow, aux_late;
} s_vstat;
static int s_vstats_every;
static int s_batch = 1;
static uint32_t s_flushes, s_flush_bytes, s_blocks;
static uint64_t s_flush_ticks;

int plat_voice_channels(void) { return s_voices_on ? VOICE_CHANNELS : 0; }
void plat_voice_set_lead(uint32_t frames) { s_voice_lead = frames; }

static VoiceEvent* ev_push(uint8_t type, int ch) {
    if (s_ev_head - s_ev_tail >= VOICE_EVENTS) {
        s_vstat.dropped_events++;
        return NULL;
    }
    VoiceEvent* e = &s_ev[s_ev_head % VOICE_EVENTS];
    memset(e, 0, sizeof(*e));
    e->type = type;
    e->ch = (uint8_t)ch;
    e->at = s_fill_pos + s_voice_lead;
    e->cache = -1;
    s_ev_head++;
    return e;
}

/* --- DSP-ADPCM decode and encode, for the loop re-alignment --- */

static inline int16_t clamp16(int32_t v) { return v > 32767 ? 32767 : (v < -32768 ? -32768 : (int16_t)v); }

/* Decodes samples [first, first + n) of frame data, starting at `first` with
 * the decoder history yn1/yn2 (the frame header gives each frame's ps). */
static void adpcm_decode(const uint8_t* data, const uint16_t* coefs, uint32_t first, uint32_t n, int32_t yn1,
                         int32_t yn2, int16_t* out) {
    for (uint32_t i = 0; i < n; i++) {
        uint32_t s = first + i;
        const uint8_t* fr = data + (s / 14) * 8;
        uint32_t k = s % 14;
        int ps = fr[0];
        int nib = fr[1 + k / 2];
        nib = (k & 1) ? (nib & 15) : (nib >> 4);
        if (nib >= 8) {
            nib -= 16;
        }
        int idx = (ps >> 4) & 7;
        int32_t val = (nib << (ps & 15)) << 11;
        val += (int16_t)coefs[idx * 2] * yn1 + (int16_t)coefs[idx * 2 + 1] * yn2;
        int16_t v = clamp16((val + 1024) >> 11);
        yn2 = yn1;
        yn1 = v;
        out[i] = v;
    }
}

/* Encodes 14 samples into one frame (8 bytes) with the coefficients given,
 * from the decoder history yn1/yn2. Every predictor and scale is tried; for
 * each, a beam search over the nibbles (the rounded one and its neighbours,
 * the best BEAM paths kept), every candidate run through the decoder exactly.
 * The frame with the least squared error wins (a tie: the smaller largest
 * error). Returns the new history in yn1 and yn2. */
#define BEAM 8
typedef struct {
    int32_t y1, y2;
    int64_t err;
    int32_t worst;
    uint8_t nib[14];
} EncPath;

static void adpcm_encode_frame(const int16_t* pcm, const uint16_t* coefs, int32_t* yn1, int32_t* yn2, uint8_t* out) {
    EncPath best;
    int best_ps = 0;
    best.err = INT64_MAX;
    best.worst = INT32_MAX;
    for (int idx = 0; idx < 8; idx++) {
        int32_t c1 = (int16_t)coefs[idx * 2], c2 = (int16_t)coefs[idx * 2 + 1];
        for (int scale = 0; scale < 12; scale++) {
            EncPath beam[BEAM], next[BEAM * 3];
            int nb = 1;
            beam[0].y1 = *yn1;
            beam[0].y2 = *yn2;
            beam[0].err = 0;
            beam[0].worst = 0;
            for (int k = 0; k < 14; k++) {
                int nn = 0;
                for (int b = 0; b < nb; b++) {
                    int32_t pred = c1 * beam[b].y1 + c2 * beam[b].y2; /* 11 fraction bits, as the decoder's s32 sum */
                    int64_t want = ((int64_t)pcm[k] << 11) - pred;
                    int64_t step = (int64_t)1 << (scale + 11);
                    int64_t q = (want >= 0 ? want + step / 2 : want - step / 2) / step;
                    q = q > 7 ? 7 : (q < -8 ? -8 : q);
                    for (int64_t t = q - 1; t <= q + 1; t++) {
                        if (t < -8 || t > 7) {
                            continue;
                        }
                        int32_t nib = (int32_t)t;
                        int16_t v = clamp16((((nib << scale) << 11) + pred + 1024) >> 11);
                        int32_t d = v - pcm[k];
                        EncPath* p = &next[nn++];
                        *p = beam[b];
                        p->err += (int64_t)d * d;
                        p->worst = d < 0 ? (-d > p->worst ? -d : p->worst) : (d > p->worst ? d : p->worst);
                        p->nib[k] = (uint8_t)(nib & 15);
                        p->y2 = p->y1;
                        p->y1 = v;
                    }
                }
                /* keep the BEAM best (partial selection sort; nn <= 3 * BEAM) */
                nb = nn < BEAM ? nn : BEAM;
                for (int i = 0; i < nb; i++) {
                    int m = i;
                    for (int j = i + 1; j < nn; j++) {
                        if (next[j].err < next[m].err) {
                            m = j;
                        }
                    }
                    EncPath tmp = next[i];
                    next[i] = next[m];
                    next[m] = tmp;
                    beam[i] = next[i];
                }
            }
            if (beam[0].err < best.err || (beam[0].err == best.err && beam[0].worst < best.worst)) {
                best = beam[0];
                best_ps = (idx << 4) | scale;
            }
        }
    }
    memset(out, 0, 8);
    out[0] = (uint8_t)best_ps;
    for (int k = 0; k < 14; k++) {
        out[1 + k / 2] |= (uint8_t)(best.nib[k] << ((k & 1) ? 0 : 4));
    }
    *yn1 = best.y1;
    *yn2 = best.y2;
}

/* Moves a loop that starts inside a frame to the next frame boundary (see
 * the top of this section). e: the entry, with lin holding the frames from
 * sample 0 and room for one more frame. Returns 0 when it cannot (a loop
 * shorter than two frames). */
static int realign_loop(CacheEntry* e, const uint16_t* coefs) {
    uint32_t ls = e->loop_start, end = e->end, len = end - ls;
    uint32_t nls = (ls + 13) / 14 * 14, d = nls - ls;
    if (d == 0) {
        return 1;
    }
    if (len < 28 || nls >= end) {
        return 0;
    }
    /* The loop as the decoder plays it after a jump: from ls with the loop history. */
    static int16_t pcm[16 * 14 + 32];
    int16_t head[14];
    adpcm_decode(e->lin, coefs, ls, d, e->loop_yn1, e->loop_yn2, head);
    /* The history at the frame holding the old end, decoded from the loop start
     * (the same values the first pass reaches: the loop history is the first
     * pass's own). */
    uint32_t f = end / 14; /* first frame to re-encode: holds sample `end` */
    uint32_t nend = end + d;
    uint32_t last = (nend - 1) / 14;
    int32_t h1, h2;
    {
        int32_t y1 = e->loop_yn1, y2 = e->loop_yn2;
        uint32_t s = ls;
        int16_t buf[256];
        while (s < f * 14) {
            uint32_t n = f * 14 - s;
            n = n > 256 ? 256 : n;
            adpcm_decode(e->lin, coefs, s, n, y1, y2, buf);
            y2 = n >= 2 ? buf[n - 2] : y1;
            y1 = buf[n - 1];
            s += n;
        }
        h1 = y1;
        h2 = y2;
    }
    /* Target samples for frames f..last: the original up to end, then the loop's head. */
    uint32_t nsmp = (last - f + 1) * 14;
    if (nsmp > sizeof(pcm) / sizeof(pcm[0])) {
        return 0;
    }
    adpcm_decode(e->lin, coefs, f * 14, end - f * 14, h1, h2, pcm);
    for (uint32_t i = end - f * 14, j = 0; i < nsmp; i++, j++) {
        pcm[i] = j < d ? head[j] : 0;
    }
    int32_t y1 = h1, y2 = h2;
    for (uint32_t fr = f; fr <= last; fr++) {
        adpcm_encode_frame(pcm + (fr - f) * 14, coefs, &y1, &y2, e->lin + fr * 8);
    }
    /* The new loop's history: the samples just before nls, as the loop plays them. */
    e->loop_yn2 = d >= 2 ? head[d - 2] : e->loop_yn1;
    e->loop_yn1 = head[d - 1];
    e->loop_ps = e->lin[(nls / 14) * 8];
    e->loop_start = nls;
    e->end = nend;
    e->realigned = 1;
    return 1;
}

static void cache_free(CacheEntry* e) {
    if (e->lin != NULL) {
        linearFree(e->lin);
        s_cache_bytes -= e->lin_bytes;
    }
    memset(e, 0, sizeof(*e));
}

/* An entry for this sample, copied and checked. Returns its index or -1. */
static int cache_get(const PlatVoiceSample* smp) {
    uint32_t end = smp->end;
    uint32_t frames = (end + 13) / 14 + 2; /* one spare frame for the re-alignment */
    uint32_t src_bytes = ((end + 13) / 14) * 8;
    s_cache_clock++;
    int free_slot = -1;
    for (int i = 0; i < CACHE_ENTRIES; i++) {
        CacheEntry* e = &s_cache[i];
        if (e->key == 0) {
            if (free_slot < 0) {
                free_slot = i;
            }
            continue;
        }
        if (e->key == smp->key && e->src == smp->adpcm && e->want_end == end && e->want_loop == smp->loop_start &&
            e->looping == smp->looping && e->src_bytes == src_bytes) {
            /* The re-encoded frames differ from the source: compare the frames before them. */
            uint32_t cmp = e->realigned ? (smp->end / 14) * 8 : src_bytes;
            if (memcmp(e->lin, smp->adpcm, cmp) == 0) {
                s_vstat.hits++;
                e->last_use = s_cache_clock;
                return i;
            }
            if (e->users == 0) {
                cache_free(e); /* stale: the game loaded something else there */
                if (free_slot < 0) {
                    free_slot = i;
                }
            }
        }
    }
    s_vstat.misses++;
    uint32_t bytes = frames * 8;
    /* LRU eviction: over the budget, or no slot */
    while (free_slot < 0 || s_cache_bytes + bytes > s_cache_budget) {
        int victim = -1;
        for (int i = 0; i < CACHE_ENTRIES; i++) {
            if (s_cache[i].key != 0 && s_cache[i].users == 0 &&
                (victim < 0 || (int32_t)(s_cache[i].last_use - s_cache[victim].last_use) < 0)) {
                victim = i;
            }
        }
        if (victim < 0) {
            return -1; /* everything is playing */
        }
        cache_free(&s_cache[victim]);
        s_vstat.evicted++;
        if (free_slot < 0) {
            free_slot = victim;
        }
    }
    CacheEntry* e = &s_cache[free_slot];
    e->lin = linearAlloc(bytes);
    if (e->lin == NULL) {
        return -1;
    }
    e->lin_bytes = bytes;
    s_cache_bytes += bytes;
    memcpy(e->lin, smp->adpcm, src_bytes);
    memset(e->lin + src_bytes, 0, bytes - src_bytes);
    e->key = smp->key;
    e->src = smp->adpcm;
    e->src_bytes = src_bytes;
    e->want_end = end;
    e->want_loop = smp->loop_start;
    e->end = end;
    e->loop_start = smp->loop_start;
    e->looping = smp->looping;
    e->loop_ps = smp->loop_ps;
    e->loop_yn1 = smp->loop_yn1;
    e->loop_yn2 = smp->loop_yn2;
    e->last_use = s_cache_clock;
    if (e->looping && e->loop_start % 14 != 0) {
        uint32_t ls = e->loop_start, end = e->end;
        if (!realign_loop(e, smp->coefs)) {
            cache_free(e);
            return -1;
        }
        if (s_vstat.realigned++ < 64 && s_vstats_every > 0) {
            PLOG_I("[NDSPV] loop re-aligned: ARAM %08lx, loop %lu..%lu -> %lu..%lu", (unsigned long)e->key,
                   (unsigned long)ls, (unsigned long)end, (unsigned long)e->loop_start, (unsigned long)e->end);
        }
    } else if (e->looping) {
        e->loop_ps = e->lin[(e->loop_start / 14) * 8];
    }
    DSP_FlushDataCache(e->lin, bytes);
    return free_slot;
}

int plat_voice_start(int ch, const PlatVoiceSample* smp, const PlatVoiceMix* mix) {
    if (!s_voices_on || ch < 1 || ch > VOICE_CHANNELS || smp->start % 14 != 0 || smp->end <= smp->start ||
        (smp->looping && (smp->loop_start >= smp->end))) {
        s_vstat.refused++;
        return 0;
    }
    int c = cache_get(smp);
    if (c >= 0 && smp->start >= s_cache[c].end) {
        c = -1;
    }
    if (c < 0) {
        s_vstat.refused++;
        return 0;
    }
    VoiceEvent* e = ev_push(EV_START, ch);
    if (e == NULL) {
        return 0;
    }
    s_cache[c].users++;
    e->cache = (int16_t)c;
    e->start = smp->start;
    e->mix = *mix;
    memcpy(e->coefs, smp->coefs, sizeof(e->coefs));
    e->ps = smp->ps;
    e->yn1 = smp->yn1;
    e->yn2 = smp->yn2;
    s_vstat.starts++;
    return 1;
}

void plat_voice_mix(int ch, const PlatVoiceMix* mix) {
    if (s_voices_on && ch >= 1 && ch <= VOICE_CHANNELS) {
        VoiceEvent* e = ev_push(EV_MIX, ch);
        if (e != NULL) {
            e->mix = *mix;
        }
    }
}

void plat_voice_stop(int ch) {
    if (s_voices_on && ch >= 1 && ch <= VOICE_CHANNELS) {
        ev_push(EV_STOP, ch);
    }
}

void plat_voice_note(uint32_t software, uint32_t ndsp, uint32_t aux, uint32_t overflow, uint32_t aux_late) {
    s_vstat.sw = software;
    s_vstat.ndsp = ndsp;
    s_vstat.aux = aux;
    s_vstat.overflow = overflow;
    s_vstat.aux_late = aux_late;
}

/* --- NDSP streams --- */

static int s_streams_on;
static StreamSlot s_str[STREAM_SLOTS];
static StreamEvent s_sev[STREAM_EVENTS];
static uint32_t s_sev_head, s_sev_tail;
static uint16_t s_str_gen;
static uint32_t s_wake; /* DSP frames seen by the audio thread */
static Thread s_audio_thread_id;
static struct {
    uint32_t starts, refills, refill_bytes, underruns, waits, dropped;
} s_sstat;

static void stream_free(StreamSlot* st) {
    if (st->mirror != NULL) {
        linearFree(st->mirror);
    }
    memset(st, 0, sizeof(*st));
}

int plat_stream_start(int ch, const PlatStream* ps, const PlatVoiceMix* mix) {
    if (!s_streams_on || ch < 1 || ch > VOICE_CHANNELS || ps->samples < 28 || ps->bytes == 0 ||
        (!ps->pcm16 && ps->samples % 14 != 0)) {
        return 0;
    }
    int i;
    for (i = 0; i < STREAM_SLOTS && s_str[i].used; i++) {
    }
    if (i == STREAM_SLOTS) {
        return 0;
    }
    StreamSlot* st = &s_str[i];
    memset(st, 0, sizeof(*st));
    st->mirror = linearAlloc((ps->bytes + 31) & ~31u);
    if (st->mirror == NULL) {
        return 0;
    }
    VoiceEvent* e = ev_push(EV_START, ch);
    if (e == NULL) {
        linearFree(st->mirror);
        st->mirror = NULL;
        return 0;
    }
    memcpy(st->mirror, ps->ring, ps->bytes);
    DSP_FlushDataCache(st->mirror, ps->bytes);
    st->used = 1;
    st->users = 1; /* the start event's reference */
    st->pcm16 = ps->pcm16;
    st->first = 1;
    st->ch = (uint8_t)ch;
    st->gen = ++s_str_gen;
    st->ring = ps->ring;
    st->bytes = ps->bytes;
    st->samples = ps->samples;
    st->ps = ps->ps;
    e->cache = -2 - i; /* a stream start: the slot */
    e->mix = *mix;
    memcpy(e->coefs, ps->coefs, sizeof(e->coefs));
    s_vch[ch].str_latest = (int8_t)i;
    s_sstat.starts++;
    return 1;
}

void plat_stream_refill(int ch, uint32_t off, uint32_t len) {
    if (!s_streams_on || ch < 1 || ch > VOICE_CHANNELS || s_vch[ch].str_latest < 0 || threadGetCurrent() != s_audio_thread_id) {
        return;
    }
    StreamSlot* st = &s_str[s_vch[ch].str_latest];
    if (!st->used || off >= st->bytes) {
        return;
    }
    if (len > st->bytes - off) {
        len = st->bytes - off;
    }
    if (s_sev_head - s_sev_tail >= STREAM_EVENTS) {
        s_sstat.dropped++;
        return;
    }
    StreamEvent* e = &s_sev[s_sev_head % STREAM_EVENTS];
    /* the refill ends where MusyX's position is at the end of this quantum's block (one quantum after
     * its output time); the DSP is allowed a frame of slack, and the apply test is one quantum early */
    e->at = s_fill_pos + s_voice_lead + 4 * s_quantum;
    e->slot = (uint8_t)(st - s_str);
    e->gen = st->gen;
    e->off = off;
    e->len = len;
    s_sev_head++;
}

/* The channel's stream, closing: the DSP may still read its mirror for a frame or two. */
static void stream_release(int ch) {
    VoiceChannel* vc = &s_vch[ch];
    if (vc->stream >= 0) {
        StreamSlot* st = &s_str[vc->stream];
        st->free_wake = s_wake + 4;
        st->used = 2; /* closing */
        vc->stream = -1;
    }
}

static void stream_reap(void) {
    for (int i = 0; i < STREAM_SLOTS; i++) {
        if (s_str[i].used == 2 && (int32_t)(s_wake - s_str[i].free_wake) >= 0) {
            stream_free(&s_str[i]);
        }
    }
}

/* Queues segments of a channel's stream as the DSP finishes them. */
static void stream_pump(int ch) {
    VoiceChannel* vc = &s_vch[ch];
    if (vc->stream < 0) {
        return;
    }
    StreamSlot* st = &s_str[vc->stream];
    int pending = 0, any_done = 0;
    uint32_t q = st->abs_next;
    int have_q = 0;
    for (int i = 0; i < STREAM_SEGS; i++) {
        int s = st->wb[i].status;
        if (s == NDSP_WBUF_PLAYING) {
            q = st->wb_abs[i] + ndspChnGetSamplePos(ch);
            have_q = 1;
            pending++;
        } else if (s == NDSP_WBUF_QUEUED) {
            if (!have_q || (int32_t)(st->wb_abs[i] - q) < 0) {
                q = st->wb_abs[i];
            }
            pending++;
        } else if (s == NDSP_WBUF_DONE) {
            any_done = 1;
        }
    }
    if (pending == 0 && !st->first && any_done) {
        s_sstat.underruns++;
    }
    uint32_t guard = st->samples * 2 / 5;
    while (pending < STREAM_DEPTH) {
        int i;
        for (i = 0; i < STREAM_SEGS && (st->wb[i].status == NDSP_WBUF_QUEUED || st->wb[i].status == NDSP_WBUF_PLAYING); i++) {
        }
        if (i == STREAM_SEGS) {
            break;
        }
        uint32_t rp = st->abs_next % st->samples, len = st->samples - rp;
        if (len > STREAM_SEG) {
            len = STREAM_SEG;
        }
        if (!st->first && st->abs_next + len - q > st->samples - guard) {
            s_sstat.waits++;
            break;
        }
        ndspWaveBuf* wb = &st->wb[i];
        memset(wb, 0, sizeof(*wb));
        if (st->pcm16) {
            wb->data_pcm16 = (int16_t*)(st->mirror + rp * 2);
        } else {
            wb->data_adpcm = st->mirror + (rp / 14) * 8;
            if (st->first) {
                st->ctx.index = st->ps;
                st->ctx.history0 = 0;
                st->ctx.history1 = 0;
                wb->adpcm_data = &st->ctx;
            } /* later buffers: the decoder carries its history on */
        }
        wb->nsamples = len;
        wb->looping = false;
        st->wb_abs[i] = st->abs_next;
        if (st->first) {
            q = st->abs_next;
            st->first = 0;
        }
        ndspChnWaveBufAdd(ch, wb);
        st->abs_next += len;
        pending++;
    }
}

static void apply_sdata(const StreamEvent* e) {
    StreamSlot* st = &s_str[e->slot];
    if (st->used != 1 || st->gen != e->gen) {
        return;
    }
    memcpy(st->mirror + e->off, st->ring + e->off, e->len);
    DSP_FlushDataCache(st->mirror + e->off, e->len);
    s_sstat.refills++;
    s_sstat.refill_bytes += e->len;
}

static void apply_mix(int ch, const PlatVoiceMix* m) {
    VoiceChannel* vc = &s_vch[ch];
    set_mix(ch, m->l, m->r);
    ndspChnSetRate(ch, m->rate);
    if (m->interp != vc->interp) {
        vc->interp = m->interp;
        ndspChnSetInterp(ch, m->interp == 1 ? NDSP_INTERP_LINEAR : m->interp >= 2 ? NDSP_INTERP_NONE : NDSP_INTERP_POLYPHASE);
    }
    if (m->lpf != vc->lpf_on || (m->lpf && (m->lpf_a0 != vc->lpf_a0 || m->lpf_b0 != vc->lpf_b0))) {
        vc->lpf_on = m->lpf;
        vc->lpf_a0 = m->lpf_a0;
        vc->lpf_b0 = m->lpf_b0;
        if (m->lpf) {
            /* MusyX: y = (a0 x + b0 y') / 32768; NDSP: y = (b0 x - a1 y') / a0 */
            ndspChnIirMonoSetParamsCustomFilter(ch, 1.0f, -(float)m->lpf_b0 / 32768.0f, (float)m->lpf_a0 / 32768.0f);
        }
        ndspChnIirMonoSetEnable(ch, m->lpf != 0);
    }
}

static void channel_release(int ch) {
    VoiceChannel* vc = &s_vch[ch];
    if (vc->cache >= 0) {
        s_cache[vc->cache].users--;
        vc->cache = -1;
    }
    stream_release(ch);
}

static void apply_event(const VoiceEvent* e) {
    int ch = e->ch;
    VoiceChannel* vc = &s_vch[ch];
    switch (e->type) {
    case EV_STOP:
        ndspChnWaveBufClear(ch);
        channel_release(ch);
        break;
    case EV_MIX:
        if (vc->cache >= 0 || vc->stream >= 0) {
            apply_mix(ch, &e->mix);
        }
        break;
    case EV_START: {
        if (e->cache <= -2) {
            int slot = -2 - e->cache;
            StreamSlot* st = &s_str[slot];
            ndspChnWaveBufClear(ch);
            channel_release(ch);
            vc->stream = (int8_t)slot; /* the start event's reference moves to the channel */
            ndspChnSetFormat(ch, st->pcm16 ? NDSP_FORMAT_MONO_PCM16 : NDSP_FORMAT_MONO_ADPCM);
            if (!st->pcm16) {
                ndspChnSetAdpcmCoefs(ch, (u16*)e->coefs);
            }
            apply_mix(ch, &e->mix);
            stream_pump(ch);
            break;
        }
        CacheEntry* c = &s_cache[e->cache];
        ndspChnWaveBufClear(ch);
        channel_release(ch);
        vc->cache = e->cache; /* the start event's reference moves to the channel */
        vc->set ^= 1;
        ndspWaveBuf* wb = vc->wb[vc->set];
        ndspAdpcmData* ctx = vc->ctx[vc->set];
        memset(wb, 0, sizeof(vc->wb[0]));
        ndspChnSetFormat(ch, NDSP_FORMAT_MONO_ADPCM);
        ndspChnSetAdpcmCoefs(ch, (u16*)e->coefs);
        apply_mix(ch, &e->mix);
        /* A: the part before the loop (or the whole one-shot, or from a start
         * past the loop start to the end); B: the loop, repeated */
        if (!c->looping || e->start != c->loop_start) {
            ctx[0].index = e->ps;
            ctx[0].history0 = e->yn1;
            ctx[0].history1 = e->yn2;
            wb[0].data_adpcm = c->lin + (e->start / 14) * 8;
            wb[0].nsamples = (c->looping && e->start < c->loop_start ? c->loop_start : c->end) - e->start;
            wb[0].adpcm_data = &ctx[0];
            wb[0].looping = false;
            ndspChnWaveBufAdd(ch, &wb[0]);
        }
        if (c->looping) {
            ctx[1].index = c->loop_ps;
            ctx[1].history0 = c->loop_yn1;
            ctx[1].history1 = c->loop_yn2;
            wb[1].data_adpcm = c->lin + (c->loop_start / 14) * 8;
            wb[1].nsamples = c->end - c->loop_start;
            wb[1].adpcm_data = &ctx[1];
            wb[1].looping = true;
            ndspChnWaveBufAdd(ch, &wb[1]);
        }
        break;
    }
    }
}

/* Channel 0's playback position in output frames: the buffer playing now and
 * the DSP's position in it. Kept when nothing plays (an underrun). */
static uint32_t play_position(void) {
    uint16_t seq = ndspChnGetWaveBufSeq(0);
    for (int i = 0; i < WAVEBUFS; i++) {
        if (s_wbuf[i].sequence_id == seq && s_wbuf[i].status != NDSP_WBUF_FREE) {
            s_play_pos = s_wbuf_pos[i] + ndspChnGetSamplePos(0);
            break;
        }
    }
    return s_play_pos;
}

static void apply_due_events(void) {
    if (s_ev_head == s_ev_tail && s_sev_head == s_sev_tail) {
        return;
    }
    uint32_t now = play_position();
    while (s_sev_tail != s_sev_head) {
        const StreamEvent* se = &s_sev[s_sev_tail % STREAM_EVENTS];
        int32_t sahead = (int32_t)(se->at - now);
        if (sahead > (int32_t)s_quantum && s_sev_head - s_sev_tail < STREAM_EVENTS / 2 && sahead < (int32_t)(s_rate / 2)) {
            break;
        }
        apply_sdata(se);
        s_sev_tail++;
    }
    /* due: at or before the next DSP frame (changes take effect a frame later) */
    while (s_ev_tail != s_ev_head) {
        const VoiceEvent* e = &s_ev[s_ev_tail % VOICE_EVENTS];
        int32_t ahead = (int32_t)(e->at - now);
        if (ahead > (int32_t)s_quantum) {
            /* never let the queue outgrow the buffering: a stalled channel 0 must not hold voices back */
            if (s_ev_head - s_ev_tail < VOICE_EVENTS / 2 && ahead < (int32_t)(s_rate / 2)) {
                break;
            }
            s_vstat.late++;
        }
        apply_event(e);
        s_ev_tail++;
    }
}

static void voices_log(void) {
    static uint64_t since;
    if (s_vstats_every <= 0 || s_frames - since < (uint64_t)s_rate * s_vstats_every) {
        return;
    }
    since = s_frames;
    int playing = 0;
    for (int ch = 1; s_voices_on && ch <= VOICE_CHANNELS; ch++) {
        playing += s_vch[ch].cache >= 0 || s_vch[ch].stream >= 0;
    }
    PLOG_I("[NDSPV] voices sw %lu ndsp %lu aux %lu overflow %lu norebuild %lu | channels playing %d | starts %lu "
           "refused %lu cache hit %lu miss %lu realigned %lu evicted %lu %lu KB | events late %lu dropped %lu",
           (unsigned long)s_vstat.sw, (unsigned long)s_vstat.ndsp, (unsigned long)s_vstat.aux,
           (unsigned long)s_vstat.overflow, (unsigned long)s_vstat.aux_late, playing, (unsigned long)s_vstat.starts,
           (unsigned long)s_vstat.refused, (unsigned long)s_vstat.hits, (unsigned long)s_vstat.misses,
           (unsigned long)s_vstat.realigned, (unsigned long)s_vstat.evicted, (unsigned long)(s_cache_bytes / 1024),
           (unsigned long)s_vstat.late, (unsigned long)s_vstat.dropped_events);
    PLOG_I("[NDSPV] flush ticks %lu (us %lu)", (unsigned long)(s_flush_ticks / 1000),
           (unsigned long)(s_flush_ticks * 1000 / (SYSCLOCK_ARM11 / 1000)));
    PLOG_I("[NDSPV] channel 0: %lu blocks, %lu flushes (%lu KB) | streams %s: starts %lu refills %lu (%lu B) "
           "underruns %lu waits %lu dropped %lu",
           (unsigned long)s_blocks, (unsigned long)s_flushes, (unsigned long)(s_flush_bytes / 1024),
           s_streams_on ? "on" : "off", (unsigned long)s_sstat.starts, (unsigned long)s_sstat.refills,
           (unsigned long)s_sstat.refill_bytes, (unsigned long)s_sstat.underruns, (unsigned long)s_sstat.waits,
           (unsigned long)s_sstat.dropped);
}

static void voices_init(void) {
    int on = plat_switch_int("ndspvoices", 0) != 0;
    s_vstats_every = plat_switch_int("ndspstats", on ? 10 : 0);
    for (int ch = 0; ch <= VOICE_CHANNELS; ch++) {
        s_vch[ch].cache = -1;
        s_vch[ch].stream = -1;
        s_vch[ch].str_latest = -1;
    }
    if (!on) {
        return;
    }
    uint32_t free_lin = linearSpaceFree();
    s_cache_budget = (uint32_t)plat_switch_int("ndspcachekb", 4096) * 1024;
    if (s_cache_budget > free_lin / 2) {
        s_cache_budget = free_lin / 2;
    }
    for (int ch = 1; ch <= VOICE_CHANNELS; ch++) {
        ndspChnReset(ch);
        ndspChnSetInterp(ch, NDSP_INTERP_POLYPHASE);
        ndspChnSetFormat(ch, NDSP_FORMAT_MONO_ADPCM);
        s_vch[ch].cache = -1;
        s_vch[ch].stream = -1;
        s_vch[ch].str_latest = -1;
        s_vch[ch].interp = 0;
    }
    s_streams_on = plat_switch_int("ndspstreams", 0) != 0;
    s_voices_on = 1;
    PLOG_I("audio: NDSP voices on, channels 1..%d, sample cache %lu KB (linear free %lu KB), streams %s", VOICE_CHANNELS,
           (unsigned long)(s_cache_budget / 1024), (unsigned long)(free_lin / 1024), s_streams_on ? "on" : "off");
}

/* Switch audiobatch=N: channel 0's buffers are refilled once N are free (N=1: each as it frees), and the
 * refilled ones are flushed in one call over the whole ring: the cache flush is a call per call whatever
 * its size, and the ring is one 16 KB block. The flush comes before the buffers are queued. */
static void ndsp_thread(void* arg) {
    (void)arg;
    plat_crash_install_thread();
    plat_thread_register("audio", 32 * 1024);
    s_audio_thread_id = threadGetCurrent();
    /* Prime the ring: every buffer queued before the first wait. */
    for (int i = 0; i < WAVEBUFS; i++) {
        s_wbuf_pos[i] = s_frames;
        fill_block(s_wbuf[i].data_pcm16);
    }
    DSP_FlushDataCache(s_samples, WAVEBUFS * s_buf_frames * 4);
    for (int i = 0; i < WAVEBUFS; i++) {
        ndspChnWaveBufAdd(0, &s_wbuf[i]);
    }
    for (;;) {
        LightEvent_Wait(&s_frame_event);
        uint64_t t0 = svcGetSystemTick();
        s_wake++;
        apply_due_events();
        if (s_streams_on) {
            stream_reap();
            for (int ch = 1; ch <= VOICE_CHANNELS; ch++) {
                stream_pump(ch);
            }
        }
        s_busy_ticks += svcGetSystemTick() - t0;
        voices_log();
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
        if (done == 0 || (done < s_batch && done < WAVEBUFS - 1)) {
            continue; /* a batch is waited for (the others still play) */
        }
        /* Refill in the order the DSP played them (sequence ids). */
        int order[WAVEBUFS], n = 0;
        for (int k = 0; k < done; k++) {
            int oldest = -1;
            for (int i = 0; i < WAVEBUFS; i++) {
                int used = 0;
                for (int j = 0; j < n; j++) {
                    used |= order[j] == i;
                }
                if (!used && s_wbuf[i].status == NDSP_WBUF_DONE &&
                    (oldest < 0 || (int16_t)(s_wbuf[i].sequence_id - s_wbuf[oldest].sequence_id) < 0)) {
                    oldest = i;
                }
            }
            if (oldest < 0) {
                break;
            }
            order[n++] = oldest;
        }
        for (int k = 0; k < n; k++) {
            s_wbuf_pos[order[k]] = s_frames;
            fill_block(s_wbuf[order[k]].data_pcm16);
            if (s_batch <= 1) {
                uint64_t f0 = svcGetSystemTick();
                DSP_FlushDataCache(s_wbuf[order[k]].data_pcm16, s_buf_frames * 4);
                s_flush_ticks += svcGetSystemTick() - f0;
                s_flushes++;
                s_flush_bytes += s_buf_frames * 4;
            }
        }
        if (s_batch > 1) {
            uint64_t f0 = svcGetSystemTick();
            DSP_FlushDataCache(s_samples, WAVEBUFS * s_buf_frames * 4);
            s_flush_ticks += svcGetSystemTick() - f0;
            s_flushes++;
            s_flush_bytes += WAVEBUFS * s_buf_frames * 4;
        }
        s_blocks += n;
        for (int k = 0; k < n; k++) {
            ndspChnWaveBufAdd(0, &s_wbuf[order[k]]);
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
    for (int ch = 0; ch <= VOICE_CHANNELS; ch++) {
        ndspChnReset(ch);
    }
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
    set_mix(0, 1.0f, 1.0f);
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
    voices_init();
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
    s_batch = plat_switch_int("audiobatch", 1);
    if (s_batch < 1) {
        s_batch = 1;
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
