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
#include "musyx/synth.h"
#include "plat/plat.h"
#include "port3ds/os_internal.h"

u8* port_aram_ptr(u32 addr);

#define DMA_BUFFER_LEN 0x280 /* 160 stereo frames, 5 ms at 32 kHz */

static SND_SOME_CALLBACK s_user_callback;
static u8 s_buffer_index;
void* salAIBufferBase;
static volatile u32 s_logic_active;
static volatile OSTime s_last_tick; /* the real clock, not the game's (a test mode can replace that) */
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
    s_last_tick = port_os_real_time();
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

void salCtrlDsp(s16* dest) {
    salBuildCommandList(dest, salGetStartDelay());
    salStartDsp(dspCmdList);
}

/* Microseconds since the last AI interrupt. */
u32 salGetStartDelay(void) { return (u32)OSTicksToMicroseconds(port_os_real_time() - s_last_tick); }

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
