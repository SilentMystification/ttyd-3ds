// Named helpers for the few places where game code writes GameCube hardware registers
// directly. With the Metrowerks compiler they expand to the original inline asm.
#pragma once

#include <dolphin/types.h>

#ifdef __MWERKS__

// Write a graphics quantization register (GQR5) used by psq_l/psq_st dequantization.
// `var` must be a u32 variable (a `register u32` on the GameCube build).
#define HW_SET_GQR5(var) asm { mtspr GQR5, var }

// Turn on MSR[FP] and MSR[RI] (0x8000 | 0x2) so the error handler can use the FPU and
// survive nested exceptions. Clobbers r5, as in the original code.
#define HW_ENABLE_MSR_FP_RI() \
    asm { \
		mfmsr	r5; \
		ori		r5, r5, 0x8000; \
		ori		r5, r5, 2; \
		mtmsr	r5; \
		isync; }

#elif defined(TARGET_PORT)

// Ports emulate the GQRs (port_fastcast.h), so the scale reaches every later
// conversion through GQR5 exactly as on the GameCube.
#include "port_fastcast.h"
#define HW_SET_GQR5(var) (port_gqr[5] = (var))
#define HW_ENABLE_MSR_FP_RI() ((void)0)

#else

#define HW_SET_GQR5(var) ((void)(var))
#define HW_ENABLE_MSR_FP_RI() ((void)0)

#endif
