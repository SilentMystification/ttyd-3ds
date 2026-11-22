/* MusyX's hardware layer (sal*, hwIrq*) for the 3DS port.
 *
 * Same structure as MusyX's GameCube layer (libs/musyx/.../hw_dolphin.c):
 * four 5 ms output blocks played by the AI (game/ai.c), the AI DMA interrupt
 * naming the next block and running MusyX's frame callback, which builds the
 * DSP command list for the block two ahead (hw_dspctrl.c). The difference is
 * the DSP: the command list is executed by the software mixer
 * (hw_pcmixer.c, MusyX's PC target, from paper-cut) on the CPU, at once, in
 * the interrupt. So the "DSP" has always finished its frame when the next
 * interrupt comes, and the frame callback always runs.
 *
 * Interrupts are the port's interrupt lock (OSDisableInterrupts), as on the
 * GameCube: the AI interrupt runs on the audio thread under it, and MusyX's
 * frame callback re-enables interrupts around the synth logic exactly as
 * hw_dolphin.c does, so the game's threads run meanwhile.
 *
 * ARAM is the port's ARAM buffer (game/ar.c); sample data in it stays
 * big-endian, as the mixer reads it.
 */
#include <dolphin/ai.h>
#include <dolphin/os.h>
#include <string.h>

#include "musyx/assert.h"
#include "musyx/dspvoice.h"
#include "musyx/hardware.h"
#include "musyx/hw_pcmixer.h"
#include "musyx/sal.h"
#include "musyx/stream.h"
#include "musyx/synth.h"
#include "plat/audio.h"
#include "plat/plat.h"

u8* port_aram_ptr(u32 addr);

#define DMA_BUFFER_LEN 0x280 /* 160 stereo frames, 5 ms at 32 kHz */

static SND_SOME_CALLBACK s_user_callback;
static u8 s_buffer_index;
void* salAIBufferBase;
static volatile u32 s_logic_active;
static volatile OSTick s_last_tick;
/* hwDisableIrq nesting, per host thread: on the GameCube one count serves
 * because the game and the audio interrupt never run at once; here the
 * interrupt runs on another core (see SoundIntOff in sound.c). */
static __thread u16 s_irq_level;
static __thread BOOL s_old_state;

/* The mixer's view of ARAM (hw_pcmixer.c, hw_aramdma.c). */
u8* portAramPtr(u32 aramAddr) { return port_aram_ptr(aramAddr); }

/* Two speakers (snd_synthapi.c asks: more would make MusyX mix surround). */
int salPcSpeakers(void) { return 2; }

u32 salGetStartDelay(void);

/* The frame callback (snd_handle_irq) with interrupts enabled, unless it is
 * already running (hw_dolphin.c callUserCallback). */
static void call_user_callback(void) {
    if (s_logic_active) {
        return;
    }
    s_logic_active = 1;
    OSEnableInterrupts();
    s_user_callback();
    OSDisableInterrupts();
    s_logic_active = 0;
}

/* AI DMA interrupt: name the next of the four blocks, then run the frame. */
static void sal_callback(void) {
    s_buffer_index = (s_buffer_index + 1) % 4;
    AIInitDMA(OSCachedToPhysical(salAIBufferBase) + s_buffer_index * DMA_BUFFER_LEN, DMA_BUFFER_LEN);
    s_last_tick = OSGetTick();
    call_user_callback();
}

bool salInitAi(SND_SOME_CALLBACK callback, u32 unk, u32* outFreq) {
    (void)unk;
    salAIBufferBase = salMallocPhysical(DMA_BUFFER_LEN * 4);
    if (salAIBufferBase == NULL) {
        return FALSE;
    }
    memset(salAIBufferBase, 0, DMA_BUFFER_LEN * 4);
    s_buffer_index = 1;
    s_logic_active = FALSE;
    s_user_callback = callback;
    AIRegisterDMACallback(sal_callback);
    AIInitDMA(OSCachedToPhysical(salAIBufferBase) + s_buffer_index * DMA_BUFFER_LEN, DMA_BUFFER_LEN);
    synthInfo.numSamples = 0x20;
    *outFreq = 32000;
    MUSY_DEBUG("MusyX AI interface initialized.\n");
    return TRUE;
}

bool salStartAi(void) {
    AIStartDMA();
    return TRUE;
}

bool salExitAi(void) {
    AIRegisterDMACallback(NULL);
    AIStopDMA();
    salFree(salAIBufferBase);
    salAIBufferBase = NULL;
    return TRUE;
}

/* The block two after the one now playing: the frame is mixed into it. */
void* salAiGetDest(void) {
    u8 index = (s_buffer_index + 2) % 4;
    return (u8*)salAIBufferBase + index * DMA_BUFFER_LEN;
}

/* No microcode to boot: the mixer's tables are built on its first frame. */
bool salInitDsp(u32 flags) {
    (void)flags;
    return TRUE;
}

bool salExitDsp(void) { return TRUE; }

/* One DSP frame, run to the end. */
void salStartDsp(u16* cmdList) {
    MUSY_ASSERT(((u32)cmdList & 0x1F) == 0);
    salPcRunCommandList(cmdList);
}

/* ---- NDSP voices (switch ndspvoices=1; plat/audio.h, docs/PORT_3DS_AUDIO.md) ----
 *
 * A voice that starts on a DSP-ADPCM sample, dry (no aux send) in studio 0,
 * gets an NDSP channel of its own when one is free, for its whole life: the
 * DSP decodes, resamples and mixes it. MusyX still runs exactly as before:
 * the command list is built, and the mixer steps that voice's parameter block
 * as its null mix does (addresses, loops, envelope and volume ramps, the
 * updates; hw_pcmixer.c salPcPbOffload), so everything MusyX reads back is
 * as with the software mix. Each frame the block's rate, envelope and volumes
 * (averaged over the frame) go to the channel. Everything else stays in the
 * software mix: PCM samples, streams, voices with an aux send (reverb,
 * chorus), and voices beyond the free channels (counted as overflow).
 *
 * Called inside salCtrlDsp, under the interrupt lock (hwIRQEnterCritical in
 * snd_handle_irq), so the game's sound calls cannot run meanwhile. */

u32 aramPcStreamBase(void);
u32 ARGetSize(void);

/* Frames between a MusyX frame and its output: the frame renders into the
 * AI block that the AI plays three quanta later (sal_callback, game/ai.c). */
#define NV_LEAD_FRAMES (3 * 160)

static int s_nv_init, s_nv_channels, s_nv_stats;
static s8 s_nv_ch[SYNTH_MAX_VOICES];    /* voice -> channel, -1: software */
static s8 s_nv_voice[24];               /* channel -> voice, -1: free */
static u8 s_nv_started[SYNTH_MAX_VOICES]; /* state 1 before this frame's list: (re)started */
static u8 s_nv_seen[24];
static struct {
    u16 env, l, r, s;
} s_nv_pre[SYNTH_MAX_VOICES];
static u32 s_nv_sw, s_nv_ndsp, s_nv_aux, s_nv_overflow, s_nv_aux_late; /* voice-frames */
/* starts kept in software, by reason (the [NDSPV] log line's "kept") */
enum { NV_KEEP_AUX, NV_KEEP_STUDIO, NV_KEEP_FORMAT, NV_KEEP_RANGE, NV_KEEP_PLAT, NV_KEEP_N };
static u32 s_nv_keep[NV_KEEP_N], s_nv_starts_seen;
/* ndspstats: where MusyX's frame goes (system ticks): the command list, the mixer */
static u64 s_t_build, s_t_mix;
static int s_nv_hint;

static int nv_voice_of(const _PB* pb) {
    int v;
    if (s_nv_hint < salNumVoices && dspVoice[s_nv_hint].pb == pb) {
        return s_nv_hint;
    }
    for (v = 0; v < salNumVoices; v++) {
        if (dspVoice[v].pb == pb) {
            s_nv_hint = v + 1;
            return v;
        }
    }
    return -1;
}

static int nv_pb_aux(const _PB* pb) {
    const _PBMIX* m = &pb->mix;
    return ((pb->mixerCtrl & 0x30) && (m->vAuxAL | m->vAuxAR | m->vDeltaAuxAL | m->vDeltaAuxAR)) ||
           ((pb->mixerCtrl & 0x80) && (m->vAuxAS | m->vDeltaAuxAS)) ||
           ((pb->mixerCtrl & 0x600) && (m->vAuxBL | m->vAuxBR | m->vDeltaAuxBL | m->vDeltaAuxBR)) ||
           ((pb->mixerCtrl & 0x1000) && (m->vAuxBS | m->vDeltaAuxBS));
}

/* The channel's rate, gains and filter from the block (env and volumes the
 * average of the frame's start, in s_nv_pre, and end). */
static void nv_mix(int v, const _PB* pb, PlatVoiceMix* m, int start_only) {
    u32 ratio = ((u32)pb->src.ratioHi << 16) | pb->src.ratioLo;
    float env = start_only ? s_nv_pre[v].env : 0.5f * ((float)s_nv_pre[v].env + pb->ve.currentVolume);
    float l = start_only ? s_nv_pre[v].l : 0.5f * ((float)s_nv_pre[v].l + pb->mix.vL);
    float r = start_only ? s_nv_pre[v].r : 0.5f * ((float)s_nv_pre[v].r + pb->mix.vR);
    float s = start_only ? s_nv_pre[v].s : 0.5f * ((float)s_nv_pre[v].s + pb->mix.vS);
    float g = env / (32768.0f * 32768.0f);
    if (!(pb->mixerCtrl & 1)) {
        l = 0;
    }
    if (!(pb->mixerCtrl & 2)) {
        r = 0;
    }
    if (!(pb->mixerCtrl & 4)) {
        s = 0;
    }
    /* the surround bus reaches the output as L = -S, R = +S (SET_OPPOSITE_LR) */
    m->l = (l - s) * g;
    m->r = (r + s) * g;
    m->rate = pb->srcSelect >= 2 ? 32000.0f : (float)ratio * (32000.0f / 65536.0f);
    m->interp = (u8)(pb->srcSelect > 2 ? 2 : pb->srcSelect);
    m->lpf = pb->lpf.on != 0;
    m->lpf_a0 = (s16)pb->lpf.a0;
    m->lpf_b0 = (s16)pb->lpf.b0;
}

/* Starts voice v on channel ch; 0 when it has to stay in software. */
static int nv_start(int v, int ch, const _PB* pb) {
    const DSPvoice* dv = &dspVoice[v];
    const SAMPLE_INFO* si = &dv->smp_info;
    const SNDADPCMinfo* info = si->extraData;
    PlatVoiceSample smp;
    PlatVoiceMix mix;
    u32 base = (u32)si->addr, end, bytes, i;
    if (info == NULL || (si->compType != 0 && si->compType != 1) || pb->addr.format != 0 || pb->loopType != 0) {
        s_nv_keep[NV_KEEP_FORMAT]++;
        return 0;
    }
    memset(&smp, 0, sizeof(smp));
    if (si->loopLength != 0) {
        smp.looping = 1;
        smp.loop_reload = 1;
        smp.loop_start = si->loop;
        end = si->loop + si->loopLength;
    } else {
        end = si->length;
    }
    bytes = (end + 13) / 14 * 8;
    if (end == 0 || base + bytes > ARGetSize() || base + bytes > aramPcStreamBase()) {
        s_nv_keep[NV_KEEP_RANGE]++;
        return 0;
    }
    smp.adpcm = port_aram_ptr(base);
    smp.key = base;
    smp.end = end;
    for (i = 0; i < 8; i++) {
        smp.coefs[i * 2] = (u16)info->coefTab[i][0];
        smp.coefs[i * 2 + 1] = (u16)info->coefTab[i][1];
    }
    if (si->compType == 1) {
        const DSPADPCMplusInfo* plus = si->extraData;
        u32 off = (si->offset + 13) / 14;
        smp.start = off * 14;
        smp.ps = plus->blk[off].PS;
        smp.yn1 = plus->blk[off].Y1;
        smp.yn2 = plus->blk[off].Y0;
    } else {
        smp.ps = info->initialPS;
    }
    if (smp.start >= end) {
        s_nv_keep[NV_KEEP_RANGE]++;
        return 0;
    }
    smp.loop_ps = info->loopPS;
    smp.loop_yn1 = info->loopY1;
    smp.loop_yn2 = info->loopY0;
    nv_mix(v, pb, &mix, 1);
    if (!plat_voice_start(ch, &smp, &mix)) {
        s_nv_keep[NV_KEEP_PLAT]++;
        return 0;
    }
    return 1;
}

static void nv_release(int ch) {
    int v = s_nv_voice[ch];
    if (v >= 0) {
        s_nv_ch[v] = -1;
    }
    s_nv_voice[ch] = -1;
    plat_voice_stop(ch);
}

static int nv_hook(_PB* pb, int phase) {
    int v = nv_voice_of(pb), ch;
    if (v < 0) {
        return 0;
    }
    if (phase == 0) {
        int aux = nv_pb_aux(pb);
        s_nv_aux += aux;
        if (s_nv_started[v]) {
            s_nv_started[v] = 0;
            if (s_nv_ch[v] >= 0) {
                nv_release(s_nv_ch[v]); /* a new note on the voice: its old sound ends */
            }
            s_nv_starts_seen++;
            if (aux) {
                s_nv_keep[NV_KEEP_AUX]++;
            } else if (dspVoice[v].studio != 0) {
                s_nv_keep[NV_KEEP_STUDIO]++;
            }
            if (s_nv_channels > 0 && !aux && dspVoice[v].studio == 0) {
                for (ch = 1; ch <= s_nv_channels && s_nv_voice[ch] >= 0; ch++) {
                }
                if (ch > s_nv_channels) {
                    s_nv_overflow++;
                } else {
                    s_nv_pre[v].env = pb->ve.currentVolume;
                    s_nv_pre[v].l = pb->mix.vL;
                    s_nv_pre[v].r = pb->mix.vR;
                    s_nv_pre[v].s = pb->mix.vS;
                    if (nv_start(v, ch, pb)) {
                        s_nv_ch[v] = (s8)ch;
                        s_nv_voice[ch] = (s8)v;
                    }
                }
            }
        }
        ch = s_nv_ch[v];
        if (ch < 0) {
            s_nv_sw += pb->state != 0;
            return 0;
        }
        s_nv_ndsp++;
        s_nv_aux_late += aux; /* an aux send that came after the start: not heard */
        s_nv_seen[ch] = 1;
        s_nv_pre[v].env = pb->ve.currentVolume;
        s_nv_pre[v].l = pb->mix.vL;
        s_nv_pre[v].r = pb->mix.vR;
        s_nv_pre[v].s = pb->mix.vS;
        return 1;
    }
    ch = s_nv_ch[v];
    if (ch >= 0) {
        if (pb->state == 0) {
            nv_release(ch); /* a one-shot's end, or MusyX stopped it in the frame */
        } else {
            PlatVoiceMix mix;
            nv_mix(v, pb, &mix, 0);
            plat_voice_mix(ch, &mix);
        }
    }
    return 1;
}

static void nv_frame_begin(void) {
    int v, ch;
    if (!s_nv_init) {
        s_nv_init = 1;
        s_nv_channels = plat_voice_channels();
        if (s_nv_channels > 23) {
            s_nv_channels = 23;
        }
        s_nv_stats = plat_switch_int("ndspstats", 0) > 0;
        for (v = 0; v < SYNTH_MAX_VOICES; v++) {
            s_nv_ch[v] = -1;
        }
        for (ch = 0; ch < 24; ch++) {
            s_nv_voice[ch] = -1;
        }
        if (s_nv_channels > 0 || s_nv_stats) {
            plat_voice_set_lead(NV_LEAD_FRAMES);
            salPcPbOffload = nv_hook;
        }
    }
    if (salPcPbOffload == NULL) {
        return;
    }
    for (v = 0; v < salNumVoices; v++) {
        s_nv_started[v] = dspVoice[v].state == 1;
    }
}

static void nv_frame_end(void) {
    int v, ch;
    if (salPcPbOffload == NULL) {
        return;
    }
    /* a voice whose block was not processed was deactivated: its channel ends */
    for (ch = 1; ch <= s_nv_channels; ch++) {
        if (s_nv_voice[ch] >= 0 && !s_nv_seen[ch]) {
            nv_release(ch);
        }
        s_nv_seen[ch] = 0;
    }
    for (v = 0; v < salNumVoices; v++) {
        s_nv_started[v] = 0;
    }
    plat_voice_note(s_nv_sw, s_nv_ndsp, s_nv_aux, s_nv_overflow, s_nv_aux_late);
    if (s_nv_stats) {
        static u32 frames;
        if (++frames % 2000 == 0) { /* every 10 s of audio */
            static u64 last;
            u64 now = plat_ticks();
            u32 wall = last ? (u32)((now - last) / 1000) : 0;
            PLOG_I("[NDSPV] voice starts %lu; kept in software: aux %lu, studio %lu, format %lu, range %lu, "
                   "refused %lu, no channel %lu; MusyX frame: list %lu.%lu%%, mixer %lu.%lu%% of the time",
                   (unsigned long)s_nv_starts_seen, (unsigned long)s_nv_keep[NV_KEEP_AUX],
                   (unsigned long)s_nv_keep[NV_KEEP_STUDIO], (unsigned long)s_nv_keep[NV_KEEP_FORMAT],
                   (unsigned long)s_nv_keep[NV_KEEP_RANGE], (unsigned long)s_nv_keep[NV_KEEP_PLAT],
                   (unsigned long)s_nv_overflow, (unsigned long)(wall ? s_t_build / wall / 10 : 0),
                   (unsigned long)(wall ? s_t_build / wall % 10 : 0), (unsigned long)(wall ? s_t_mix / wall / 10 : 0),
                   (unsigned long)(wall ? s_t_mix / wall % 10 : 0));
            last = now;
            s_t_build = s_t_mix = 0;
        }
    }
}

void salCtrlDsp(s16* dest) {
    u64 t0, t1, t2;
    nv_frame_begin();
    t0 = plat_ticks();
    salBuildCommandList(dest, salGetStartDelay());
    t1 = plat_ticks();
    salStartDsp(dspCmdList);
    t2 = plat_ticks();
    s_t_build += t1 - t0;
    s_t_mix += t2 - t1;
    nv_frame_end();
}

/* Microseconds since the last AI interrupt. */
u32 salGetStartDelay(void) { return OSTicksToMicroseconds(OSGetTick() - s_last_tick); }

void hwInitIrq(void) {
    s_old_state = OSDisableInterrupts();
    s_irq_level = 1;
}

void hwExitIrq(void) {}

void hwEnableIrq(void) {
    if (s_irq_level == 0) {
        plat_fatal("MusyX: hwEnableIrq without hwDisableIrq");
    }
    if (--s_irq_level == 0) {
        OSRestoreInterrupts(s_old_state);
    }
}

void hwDisableIrq(void) {
    if ((s_irq_level++) == 0) {
        s_old_state = OSDisableInterrupts();
    }
}

void hwIRQEnterCritical(void) { OSDisableInterrupts(); }

void hwIRQLeaveCritical(void) { OSEnableInterrupts(); }
