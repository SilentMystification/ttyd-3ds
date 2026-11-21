/* TPL texture palettes (.tpl files, filemgr type 4, UnpackTexPalette).
 *
 * Header graph only (paper-cut's port/bedecode/tpl.spec): the palette, its
 * descriptor array, and each texture and CLUT header. Texel and CLUT data
 * stay big-endian: the GX layer's texture decoder reads GameCube formats
 * from big-endian memory.
 *
 * A TPL is recognised by its version word in disc order (0x0020AF30 read
 * big-endian); a converted one reads differently, so converting twice (a
 * file reached from both filemgr and UnpackTexPalette) does nothing. */
#include "plat/plat.h"
#include "port_formats.h"
#include "port_swap_gen.h"

#define TPL_VERSION 0x0020AF30u

/* The extent of the header graph, read before converting (callers that do
 * not know the file's size). */
static u32 tpl_extent(const u8* p) {
    u32 n = port_be32(p + 4), desc = port_be32(p + 8), end = 12;
    if (n > 0x10000 || desc > PORT_SWAP_SIZE_UNKNOWN) {
        return PORT_SWAP_SIZE_UNKNOWN;
    }
    if (desc + n * 8 > end) {
        end = desc + n * 8;
    }
    for (u32 i = 0; i < n; i++) {
        u32 th = port_be32(p + desc + i * 8), ch = port_be32(p + desc + i * 8 + 4);
        if (th != 0 && th + 36 > end) {
            end = th + 36;
        }
        if (ch != 0 && ch + 12 > end) {
            end = ch + 12;
        }
    }
    return end;
}

void portTplSwap(void* data, u32 size, const char* name) {
    u8* p = data;
    if (p == NULL || port_be32(p) != TPL_VERSION) {
        return; /* converted already, or not a TPL (the game checks the version itself) */
    }
    if (size == PORT_SWAP_SIZE_UNKNOWN) {
        size = tpl_extent(p);
    }
    PortSwap s;
    port_swap_begin(&s, data, size, name);
    port_swap_struct(&s, 0, &port_swap_type_TEXPalette);
    u32 n = port_swap_get32(&s, 4), desc = port_swap_get32(&s, 8);
    port_swap_array(&s, desc, n, &port_swap_type_TEXDescriptor);
    for (u32 i = 0; i < n; i++) {
        u32 th = port_swap_get32(&s, desc + i * 8), ch = port_swap_get32(&s, desc + i * 8 + 4);
        if (th != 0) {
            port_swap_struct(&s, th, &port_swap_type_TEXHeader);
        }
        if (ch != 0) {
            port_swap_struct(&s, ch, &port_swap_type_CLUTHeader);
        }
    }
    PLOG_D("tpl %s: %lu textures", name, (unsigned long)n);
    port_swap_end(&s);
}
