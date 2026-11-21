// eff_batfukidashi.c: the recovery block's coin price bubble (コイン吹き出し): effCoinFukidashiEntry, effCoinFukidashiMain and effCoinFukidashiDisp.
#pragma once

#include "driver/camdrv.h"
#include "driver/effdrv.h"
#include <dolphin/types.h>

EffectEntry* effCoinFukidashiEntry(s32 type, s32 number, s32 duration, f32 x, f32 y, f32 z);
// Defined static in the .c file; MWCC accepts this declaration, GCC does not.
#ifndef TARGET_PORT
void effCoinFukidashiMain(EffectEntry* effect);
#endif
// Defined static in the .c file; MWCC accepts this declaration, GCC does not.
#ifndef TARGET_PORT
void effCoinFukidashiDisp(CameraId cameraId, EffectEntry* effect);
#endif
