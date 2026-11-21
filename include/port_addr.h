// Address tests that the GameCube code makes with the 0x80000000 bit.
//
// On the GameCube every pointer into main memory is 0x80000000 or above, so
// the code tells a pointer from a small id or a file offset by that bit or by
// comparing with 0x80000000. On a port, memory is at other addresses (all
// below 0x80000000 on the 3DS), so the call sites use these macros instead.
// The matching build keeps its original expressions and never includes this.
#pragma once

#include <dolphin/types.h>

#ifdef TARGET_PORT

// TRUE when w is an address in the port's memory rather than a small id. Each
// port defines port_is_addr() (port3ds/include/port_addr_impl.h); the ids the
// game passes in such words are small (event label ids, NanNPC name ids,
// sound ids), far below any port address.
#include "port_addr_impl.h"
#define PORT_IS_ADDR(w) port_is_addr((u32)(w))

// TRUE when a word in a loaded file is still a file-relative offset, not yet
// turned into an address by adding the file's base. Offsets are smaller than
// the file, every port base is above the largest file, so the comparison with
// the base itself is exact.
#define PORT_IS_UNRELOCATED(w, base) ((u32)(w) < (u32)(base))

#endif
