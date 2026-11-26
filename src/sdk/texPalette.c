// TPL texture palette helpers (SDK charPipeline): unpack a .tpl and build GXTexObjs from it.
#ifdef TARGET_PORT
#include "port_addr.h"
#include "port_formats.h"
#endif
#include <charPipeline/texPalette.h>
#include <dolphin/gx/GXTexture.h>
#include <dolphin/os.h>
#include <macros.h>

#define PALETTE_VERSION 0x0020AF30

#ifdef TARGET_PORT
// The textures of an unpacked TPL are in memory: the GX layer may convert them ahead of their first draw (texasync).
void portTplPrefetch(void* palette) {
    TEXPalettePtr pal = palette;
    u32 i;
    for (i = 0; i < pal->numDescriptors; i++) {
        TEXHeaderPtr th = pal->descriptorArray[i].textureHeader;
        if (th != NULL) {
            GXPrefetchTexPort(th->data, th->width, th->height, th->format, pal->descriptorArray[i].CLUTHeader != NULL);
        }
    }
}
#endif

/**
 * Turns a TPL loaded in memory into usable form:
 * checks the version and converts the descriptor, texture and CLUT offsets to pointers (only once).
 * Used by the effect and background drivers after loading a .tpl.
 */
void UnpackTexPalette(TEXPalettePtr pal) {
    u16 i;

#ifdef TARGET_PORT
    portTplSwap(pal, PORT_SWAP_SIZE_UNKNOWN, "tpl (UnpackTexPalette)"); // big-endian on the disc; once per file
#endif
    if (pal->versionNumber != PALETTE_VERSION) {
        OSPanic(__FILE__, 0x24, "invalid version number for texture palette\0");
    }

#ifdef TARGET_PORT
    if (PORT_IS_UNRELOCATED(pal->descriptorArray, pal)) {
#else
    if ((u32)pal->descriptorArray <= 0x80000000) {
#endif
        pal->descriptorArray = (TEXDescriptorPtr)((Ptr)pal->descriptorArray + (u32)pal);
        for (i = 0; i < pal->numDescriptors; i++) {
            if (pal->descriptorArray[i].textureHeader) {
                pal->descriptorArray[i].textureHeader =
                    (TEXHeaderPtr)((Ptr)pal + (u32)pal->descriptorArray[i].textureHeader);
                if (!pal->descriptorArray[i].textureHeader->unpacked) {
                    pal->descriptorArray[i].textureHeader->data =
                        (Ptr)pal + (u32)pal->descriptorArray[i].textureHeader->data;
                    pal->descriptorArray[i].textureHeader->unpacked = TRUE;
                }
            }
            if (pal->descriptorArray[i].CLUTHeader) {
                pal->descriptorArray[i].CLUTHeader =
                    (CLUTHeaderPtr)((u8*)pal + (u32)pal->descriptorArray[i].CLUTHeader);
                if (!pal->descriptorArray[i].CLUTHeader->unpacked) {
                    pal->descriptorArray[i].CLUTHeader->data = (Ptr)pal + (u32)pal->descriptorArray[i].CLUTHeader->data;
                    pal->descriptorArray[i].CLUTHeader->unpacked = TRUE;
                }
            }
        }
#ifdef TARGET_PORT
        portTplPrefetch(pal);
#endif
    }
}

/** Returns the descriptor for texture id (the range check is a debug-only assert). */
TEXDescriptorPtr TEXGet(TEXPalettePtr pal, u32 id) {
    ASSERTMSGLINE(0x90, id < pal->numDescriptors, "GetTexture():  Texture Not Found ");
    return &pal->descriptorArray[id];
}

/**
 * Fills a GXTexObj from texture id of a TPL: size, format, wrap,
 * filters and LOD range (mipmapped when the LODs differ).
 */
void TEXGetGXTexObjFromPalette(TEXPalettePtr pal, GXTexObj* to, u32 id) {
    TEXDescriptorPtr tdp;
    GXBool mipMap;

    tdp = TEXGet(pal, id);
    if (tdp->textureHeader->minLOD == tdp->textureHeader->maxLOD) {
        mipMap = GX_FALSE;
    } else {
        mipMap = GX_TRUE;
    }
    GXInitTexObj(to, tdp->textureHeader->data, tdp->textureHeader->width, tdp->textureHeader->height,
                 tdp->textureHeader->format, tdp->textureHeader->wrapS, tdp->textureHeader->wrapT, mipMap);
    GXInitTexObjLOD(to, tdp->textureHeader->minFilter, tdp->textureHeader->magFilter, tdp->textureHeader->minLOD,
                    tdp->textureHeader->maxLOD, tdp->textureHeader->LODBias, GX_DISABLE,
                    tdp->textureHeader->edgeLODEnable, GX_ANISO_1);
}
