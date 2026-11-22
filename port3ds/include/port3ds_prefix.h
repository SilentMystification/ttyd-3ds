/* Force-included (-include) into every C file of the 3DS build, game code
 * and port code alike. Never seen by the matching GameCube build.
 *
 * It neutralises Metrowerks CodeWarrior extensions so the decomp compiles
 * with devkitARM GCC, and defines the port macros:
 *   TARGET_PORT  any non-GameCube build of this tree (shared spelling for
 *                changes in game code that every port needs)
 *   TARGET_3DS   this port
 * Approach follows paper-cut's port/include/port_prefix.h (MIT).
 */
#ifndef PORT3DS_PREFIX_H
#define PORT3DS_PREFIX_H

#define TARGET_PORT 1
#define TARGET_3DS 1

/* __declspec(weak), __declspec(section ...): no meaning once the SDK is
 * replaced; weak SDK hooks get real definitions in port3ds/sdk. */
#define __declspec(...)

/* MWCC sizes enums between '#pragma enumsalwaysint off' and 'on' to their
 * values; GCC does the same for an enum marked packed. Headers that rely on
 * it use PORT_ENUM_PACKED after the closing brace. */
#define PORT_ENUM_PACKED __attribute__((packed))

#ifndef __ASSEMBLER__
#include <math.h>
#include <stddef.h>
#include <stdint.h>

/* Metrowerks PowerPC intrinsics. */
#define __fabs(x) __builtin_fabs(x)
#define __fabsf(x) __builtin_fabsf(x)
static inline int port3ds_abs(int x) { return x < 0 ? -x : x; }
#define __abs(x) port3ds_abs(x)
/* frsqrte is an estimate that callers refine with Newton steps. */
static inline double __frsqrte(double x) { return 1.0 / __builtin_sqrt(x); }
static inline unsigned int __cntlzw(unsigned int x) { return x == 0 ? 32u : (unsigned int)__builtin_clz(x); }

/* The game's sincosf(f32 *sin, f32 *cos, f32 degrees) is not the C library's. */
#define sincosf ttyd_sincosf

/* rand/srand are the GameCube's (MSL's LCG, port3ds/sdk/msl.c), so random
 * sequences match the original. newlib's declarations come first. */
#include <stdlib.h>
#define rand ttyd_rand
#define srand ttyd_srand
int ttyd_rand(void);
void ttyd_srand(unsigned int seed);

/* PAL's system.c defines the game's __assert (tst.rel calls it), which newlib's
 * assert.o (pulled in for __assert_func) also defines. */
#define __assert ttyd___assert
#endif

#endif
