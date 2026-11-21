// eff_energy.c: energy-orb effect: effEnergyEntry, effEnergyMain and effEnergyDisp.
#pragma once

#include "driver/camdrv.h"
#include "driver/effdrv.h"
#include <dolphin/types.h>

EffectEntry* effEnergyEntry(s32 type, s32 duration, f32 x, f32 y, f32 z, f32 size, f32 rotSpeed,
                            f32 scaleSpeed);
// Defined static in the .c file; MWCC accepts this declaration, GCC does not.
#ifndef TARGET_PORT
void effEnergyMain(EffectEntry* effect);
#endif
// Defined static in the .c file; MWCC accepts this declaration, GCC does not.
#ifndef TARGET_PORT
void effEnergyDisp(CameraId cameraId, EffectEntry* effect);
#endif
