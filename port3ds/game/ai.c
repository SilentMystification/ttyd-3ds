/* Dolphin AI (audio interface) for the 3DS port.
 *
 * The GameCube's AI plays 32 kHz stereo blocks that the CPU names with
 * AIInitDMA, and raises its DMA interrupt as each block starts, so the
 * handler can name the next one. Here the platform's audio thread
 * (plat/audio.c, NDSP) asks for samples in 5 ms quanta of 160 frames, the
 * size MusyX uses. For each quantum it
 *
 *   1. takes the block AIInitDMA last named (the block that starts now),
 *   2. raises the DMA interrupt: the registered callback runs as an
 *      interrupt handler (under the interrupt lock, port_irq_enter), and
 *      names the next block (MusyX renders two blocks ahead, as on the
 *      GameCube),
 *   3. copies the block to the output.
 *
 * Samples in the blocks are in host order (the port's mixer writes them).
 *
 * Streaming (the disc drive's audio, DTK) has its registers here too: the
 * play state, volumes, sample rate and the sample counter, which advances
 * while the stream plays as the hardware's does. TTYD never prepares a disc
 * stream (it only stops one, sound.c), so a playing stream would be silent
 * on the GameCube too, and nothing is mixed for it.
 */
#include <dolphin/ai.h>
#include <dolphin/os.h>
#include <string.h>

#include "plat/audio.h"
#include "plat/plat.h"
#include "port3ds/os_internal.h"
#include "port_addr_impl.h"

#define QUANTUM_FRAMES 160
#define QUANTUM_BYTES (QUANTUM_FRAMES * 4)

static int s_init;
static AIDCallback s_dma_callback;
static AISCallback s_stream_callback;
static volatile u32 s_dma_start, s_dma_length; /* the registers AIInitDMA sets */
static volatile u32 s_block_start, s_block_length; /* the block playing now */
static volatile BOOL s_dma_enabled;
static volatile u32 s_dsp_rate = AI_SAMPLERATE_32KHZ;
static volatile u32 s_stream_rate = AI_SAMPLERATE_48KHZ;
static volatile u32 s_stream_state;
static volatile u32 s_stream_count, s_stream_trigger;
static volatile u8 s_vol_left, s_vol_right;

static void stream_advance(void) {
    if (s_stream_state != AI_STREAM_START) {
        return;
    }
    /* 5 ms of stream samples at the stream's rate (48 or 32 kHz). */
    u32 per_quantum = s_stream_rate == AI_SAMPLERATE_48KHZ ? 240 : 160;
    u32 before = s_stream_count;
    s_stream_count = before + per_quantum;
    /* The trigger interrupt: the counter passed the trigger value. */
    if (s_stream_callback != NULL && s_stream_trigger - before - 1 < per_quantum) {
        s_stream_callback(s_stream_trigger);
    }
}

/* One quantum's interrupt: the block that starts now, the DMA callback
 * naming the next, the stream counter. Under the interrupt lock. */
static void quantum_irq(u32* start, u32* length) {
    *start = 0;
    *length = 0;
    if (s_dma_enabled) {
        s_block_start = *start = s_dma_start;
        s_block_length = *length = s_dma_length;
        if (s_dma_callback != NULL) {
            s_dma_callback();
        }
    }
    stream_advance();
}

/* Frame-paced audio (a test mode): the interrupts run from port_ai_pump on
 * the caller's thread, so sound state advances with the game frame, not
 * with the output clock; the output gets silence. */
static volatile int s_frame_paced;

void port_ai_set_frame_paced(int on) { s_frame_paced = on; }

void port_ai_pump(int quanta) {
    u32 start, length;
    while (quanta-- > 0) {
        port_irq_enter();
        quantum_irq(&start, &length);
        port_irq_leave();
    }
}

static void ai_fill(int16_t* out, uint32_t frames) {
    if (s_frame_paced) {
        memset(out, 0, frames * 4);
        return;
    }
    while (frames >= QUANTUM_FRAMES) {
        port_irq_enter();
        u32 start, length;
        quantum_irq(&start, &length); /* the interrupt names the next block */
        port_irq_leave();

        if (length > QUANTUM_BYTES) {
            length = QUANTUM_BYTES;
        }
        if (length != 0 && port_is_addr(start) && port_is_addr(start + length - 1)) {
            memcpy(out, (const void*)start, length);
        } else {
            length = 0;
        }
        memset((u8*)out + length, 0, QUANTUM_BYTES - length);
        out += QUANTUM_FRAMES * 2;
        frames -= QUANTUM_FRAMES;
    }
    if (frames != 0) {
        memset(out, 0, frames * 4);
    }
}

void AIInit(u8* stack) {
    (void)stack;
    if (s_init) {
        return;
    }
    s_init = 1;
    s_dsp_rate = AI_SAMPLERATE_32KHZ;
    s_stream_rate = AI_SAMPLERATE_48KHZ;
    s_stream_state = AI_STREAM_STOP;
    s_vol_left = s_vol_right = 0;
    s_stream_count = 0;
    plat_audio_start(32000, QUANTUM_FRAMES, ai_fill);
}

BOOL AICheckInit(void) { return s_init; }

void AIReset(void) { s_init = 0; }

AIDCallback AIRegisterDMACallback(AIDCallback callback) {
    BOOL level = OSDisableInterrupts();
    AIDCallback old = s_dma_callback;
    s_dma_callback = callback;
    OSRestoreInterrupts(level);
    return old;
}

void AIInitDMA(u32 start_addr, u32 length) {
    BOOL level = OSDisableInterrupts();
    /* The GameCube's registers hold 32-byte units. */
    s_dma_start = start_addr & ~0x1Fu;
    s_dma_length = length & ~0x1Fu;
    OSRestoreInterrupts(level);
}

BOOL AIGetDMAEnableFlag(void) { return s_dma_enabled; }

void AIStartDMA(void) { s_dma_enabled = TRUE; }

void AIStopDMA(void) { s_dma_enabled = FALSE; }

/* The quantum in flight plays out at once on the port (it was copied whole). */
u32 AIGetDMABytesLeft(void) { return 0; }

u32 AIGetDMAStartAddr(void) { return s_dma_start; }

u32 AIGetDMALength(void) { return s_dma_length; }

AISCallback AIRegisterStreamCallback(AISCallback callback) {
    BOOL level = OSDisableInterrupts();
    AISCallback old = s_stream_callback;
    s_stream_callback = callback;
    OSRestoreInterrupts(level);
    return old;
}

u32 AIGetStreamSampleCount(void) { return s_stream_count; }

void AIResetStreamSampleCount(void) { s_stream_count = 0; }

void AISetStreamTrigger(u32 trigger) { s_stream_trigger = trigger; }

u32 AIGetStreamTrigger(void) { return s_stream_trigger; }

void AISetStreamPlayState(u32 state) { s_stream_state = state; }

u32 AIGetStreamPlayState(void) { return s_stream_state; }

void AISetDSPSampleRate(u32 rate) {
    if (rate != AI_SAMPLERATE_32KHZ) {
        /* MusyX and the game run the DSP at 32 kHz; the output is fixed there. */
        PLOG_W("AISetDSPSampleRate(%lu): the port's output stays at 32 kHz", (unsigned long)rate);
    }
    s_dsp_rate = rate;
}

u32 AIGetDSPSampleRate(void) { return s_dsp_rate; }

void AISetStreamSampleRate(u32 rate) { s_stream_rate = rate; }

u32 AIGetStreamSampleRate(void) { return s_stream_rate; }

void AISetStreamVolLeft(u8 vol) { s_vol_left = vol; }

u8 AIGetStreamVolLeft(void) { return s_vol_left; }

void AISetStreamVolRight(u8 vol) { s_vol_right = vol; }

u8 AIGetStreamVolRight(void) { return s_vol_right; }
