// Data that the decomp writes in its sources (images) but that ports read from the user's disc image instead, so a
// port binary contains no game data (docs/PORT_3DS_PLAN.md 6.1). On ports the array is declared without contents
// and PORT_DISC_DATA registers it; the port fills it at boot from main.dol or the REL in the image, at the address
// tools/port3ds/gen_disc_data.py found for the version. The matching build keeps the initializer.
#pragma once

#ifdef TARGET_PORT
#include "port_disc_data_gen.h"

void port_disc_data_register(void* dst, const char* rel, int section, unsigned int offset, unsigned int size);

#define PORT_DISC_DATA_REGISTER(var, ...) port_disc_data_register(var, __VA_ARGS__)
/* A function that runs once after every array is filled (byte swaps and the like). */
void port_disc_data_add_fixup(void (*fn)(void));
#define PORT_DISC_DATA_FIXUP(fn)                                              \
    __attribute__((constructor)) static void port_disc_fixup_##fn(void) { port_disc_data_add_fixup(fn); }
#define PORT_DISC_DATA(var, key)                                           \
    __attribute__((constructor)) static void port_disc_register_##var(void) { \
        PORT_DISC_DATA_REGISTER(var, PORT_DISC_##key);                       \
    }
#endif
