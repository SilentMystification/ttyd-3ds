/* Parts of the GameCube C library (Metrowerks MSL) the game depends on
 * beyond what newlib provides identically.
 *
 * rand/srand: MSL's linear congruential generator, so random sequences
 * (enemy behaviour, item drops, the lottery) follow the original. Game code
 * calls rand/srand, renamed to these by port3ds_prefix.h.
 * __lower_map: MSL's tolower table, used by the ARC archive lookup.
 */
#include <dolphin/types.h>

#include "test_mode.h"

static u32 s_next = 1;

int ttyd_rand(void) {
    s_next = s_next * 1103515245u + 12345u;
    u32 v = (s_next >> 16) & 0x7FFF;
    tst_rngtrace((unsigned long)__builtin_return_address(0), v);
    return (int)v;
}

void ttyd_srand(unsigned int seed) { s_next = seed; }

/* The generator's state, for the test mode's gwdump lines. */
u32 ttyd_rand_state(void) { return s_next; }

/* MSL's "C" locale table: A-Z map to a-z, everything else to itself. */
#define L(c) ((c) >= 'A' && (c) <= 'Z' ? (c) + 32 : (c))
#define ROW(b) L(b + 0), L(b + 1), L(b + 2), L(b + 3), L(b + 4), L(b + 5), L(b + 6), L(b + 7), \
               L(b + 8), L(b + 9), L(b + 10), L(b + 11), L(b + 12), L(b + 13), L(b + 14), L(b + 15)
unsigned char __lower_map[256] = {
    ROW(0x00), ROW(0x10), ROW(0x20), ROW(0x30), ROW(0x40), ROW(0x50), ROW(0x60), ROW(0x70),
    ROW(0x80), ROW(0x90), ROW(0xA0), ROW(0xB0), ROW(0xC0), ROW(0xD0), ROW(0xE0), ROW(0xF0),
};

/* The REL modules' constructor and destructor tables: TTYD's RELs are C,
 * so both are empty lists (the GameCube linker makes them the same way). */
typedef void (*voidfunctionptr)(void);
voidfunctionptr _ctors[1] = {0};
voidfunctionptr _dtors[1] = {0};
