/* Binary fonts (<root>/f/<name>.bfn, fontmgr.c JUTFontSetup).
 *
 * Pointer-free: a header, then a chain of {magic, size} blocks (INF1, GLY1,
 * MAP1, WID1), after paper-cut's port/src/common/font_pc.c. Converted: the
 * header, each block's header, and MAP1's u16 table (to the end of the
 * block). WID1 entries are byte pairs; the GLY1 sheet (GX I4 texels) stays
 * big-endian for the GX layer. Recognised as unconverted by the "FONT"
 * magic in disc order. */
#include "plat/plat.h"
#include "port_formats.h"
#include "port_swap_gen.h"

#define FOURCC(a, b, c, d) ((u32)(a) << 24 | (u32)(b) << 16 | (u32)(c) << 8 | (u32)(d))

void portFontSwap(void* data, u32 size, const char* name) {
    u8* p = data;
    if (p == NULL || size < port_swap_type_BinaryFontHeader.size || port_be32(p) != FOURCC('F', 'O', 'N', 'T')) {
        return;
    }
    u64 t0 = port_conv_begin();
    PortSwap s;
    port_swap_begin(&s, data, size, name);
    port_swap_struct(&s, 0, &port_swap_type_BinaryFontHeader);
    u32 count = port_swap_get32(&s, 0xC);
    u32 off = port_swap_type_BinaryFontHeader.size;
    for (u32 i = 0; i < count; i++) {
        if (!port_swap_in(&s, off, 8)) {
            port_swap_bad(&s, "block", off, 8);
        }
        u32 magic = port_swap_get32(&s, off), bsize = port_swap_get32(&s, off + 4);
        if (bsize < 8 || !port_swap_in(&s, off, bsize)) {
            port_swap_bad(&s, "block size", off, bsize);
        }
        const PortSwapType* t = &port_swap_type_BinaryFontBlock;
        switch (magic) {
        case FOURCC('I', 'N', 'F', '1'): t = &port_swap_type_BinaryFontInfo; break;
        case FOURCC('G', 'L', 'Y', '1'): t = &port_swap_type_BinaryFontGlyph; break;
        case FOURCC('M', 'A', 'P', '1'): t = &port_swap_type_BinaryFontMap; break;
        case FOURCC('W', 'I', 'D', '1'): t = &port_swap_type_BinaryFontWidth; break;
        }
        if (bsize < t->size) {
            t = &port_swap_type_BinaryFontBlock;
        }
        port_swap_struct(&s, off, t);
        if (t == &port_swap_type_BinaryFontMap) {
            /* table[]: u16 entries to the end of the block */
            u32 head = port_swap_type_BinaryFontMap.size;
            port_swap_units(&s, off + head, (bsize - head) / 2, 2);
        }
        off += bsize;
    }
    PLOG_D("font %s: %lu blocks", name, (unsigned long)count);
    port_swap_end(&s);
    port_conv_end(PORT_CONV_FONT, t0, name, data, size);
}
