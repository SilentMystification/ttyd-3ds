/* 3DS address ranges for PORT_IS_ADDR (include/port_addr.h).
 *
 * An application process has its executable image from 0x00100000 up to
 * the end of its .bss (__end__; the build and plat/boot.c require it to be
 * at most 0x01000000, because sound ids with flag bits start at 0x01000000
 * (PSND_ID_RELATIVE); big port tables are heap allocated to stay under it,
 * see game/gx/gx_bigmem.h), and its
 * heap from 0x08000000, linear memory at 0x14000000 or 0x30000000, QTM at
 * 0x1E800000 and VRAM at 0x1F000000. Nothing is mapped between the end of
 * the image and 0x08000000.
 *
 * The ids the game passes in pointer-or-id words are small (event label
 * ids, NanNPC name ids) or sound ids with flag bits up to 0x07FFFFFF
 * (PaperSoundIdBits): those fall below 0x00100000 or in the unmapped gap,
 * so the test is exact. */
#ifndef PORT_ADDR_IMPL_H
#define PORT_ADDR_IMPL_H

extern char __end__[];

static inline int port_is_addr(unsigned int w) {
    return (w >= 0x00100000u && w < (unsigned int)__end__) || (w >= 0x08000000u && w < 0x40000000u);
}

#endif
