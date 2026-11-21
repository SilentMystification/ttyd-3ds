// eff_irekaeru.c: swap effect: effIrekaeruEntry, effIrekaeruMain and effIrekaeruDisp.
#pragma once

#include "driver/camdrv.h"
#include "driver/effdrv.h"
#include <dolphin/types.h>

EffectEntry* effIrekaeruEntry(s32 type, f32 x, f32 y, f32 z);
// Defined static in the .c file; MWCC accepts this declaration, GCC does not.
#ifndef TARGET_PORT
void effIrekaeruMain(EffectEntry* effect);
#endif
// Defined static in the .c file; MWCC accepts this declaration, GCC does not.
#ifndef TARGET_PORT
void effIrekaeruDisp(CameraId cameraId, EffectEntry* effect);
#endif
