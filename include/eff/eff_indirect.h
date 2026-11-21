// eff_indirect.c: screen-distortion effect: effIndirectEntry, effIndirectMain and effIndirectDisp.
#pragma once

#include "driver/camdrv.h"
#include "driver/effdrv.h"
#include <dolphin/types.h>

EffectEntry* effIndirectEntry(s32 type, s32 duration, f32 x, f32 y, f32 z);
// Defined static in the .c file; MWCC accepts this declaration, GCC does not.
#ifndef TARGET_PORT
void effIndirectMain(EffectEntry* effect);
#endif
// Defined static in the .c file; MWCC accepts this declaration, GCC does not.
#ifndef TARGET_PORT
void effIndirectDisp(CameraId cameraId, EffectEntry* effect);
#endif
