// Disc formats converted to host byte order on ports, right after they are read (port3ds/game/formats/). Each call
// converts a file once: a second call on the same data does nothing. `size` is the file's size, or
// PORT_SWAP_SIZE_UNKNOWN; `name` is for error messages.
#pragma once

#ifdef TARGET_PORT
#include "port3ds/port_swap.h"

void portTplSwap(void* data, u32 size, const char* name);
void portAnimSwap(void* data, u32 size, const char* name);
void portFontSwap(void* data, u32 size, const char* name);
void portMapSwap(void* data, u32 size, const char* name);
void portCamRoadSwap(void* data, u32 size, const char* name);
// A file made only of 16-bit words (icon.bin).
void portSwapU16Run(void* data, u32 count);
// Runs of 32-bit words (float and s32 tables).
void portSwapU32Run(void* data, u32 count);
// A plain texture is in memory and will probably be drawn soon (port3ds/game/gx/gx_tex.c, switch texasync).
// An unpacked TPL (TEXPalette*): queues its textures for GXPrefetchTexPort.
void portTplPrefetch(void* palette);
void GXPrefetchTexPort(const void* image, u16 width, u16 height, u32 format, int hasClut);
#endif
