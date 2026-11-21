/* 3DS address ranges for PORT_IS_ADDR (include/port_addr.h).
 *
 * An application process has its code, data and heap from 0x00100000 up,
 * linear memory at 0x14000000 or 0x30000000, QTM at 0x1E800000 and VRAM at
 * 0x1F000000: everything a pointer can hold is in [0x00100000, 0x40000000).
 * The ids the game passes in pointer-or-id words are far below 0x00100000. */
#ifndef PORT_ADDR_IMPL_H
#define PORT_ADDR_IMPL_H

static inline int port_is_addr(unsigned int w) {
    return w >= 0x00100000u && w < 0x40000000u;
}

#endif
