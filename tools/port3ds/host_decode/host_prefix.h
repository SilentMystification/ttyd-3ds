/* Force-included (-include) into every C file of the host decode tool, in place of port3ds/include/port3ds_prefix.h.
 *
 * The converters (port3ds/game/formats/, port_swap.c, musyx_swap.c) are written for a 32-bit target where the
 * decomp's `u32` is `unsigned long`. On a 64-bit host that would be 64 bits wide, so this header defines the
 * decomp's integer types itself, with their fixed widths, and keeps dolphin/types.h (which would redefine them
 * and pull in the decomp's libc wrappers) from being read. It also neutralises the Metrowerks extensions that
 * port3ds_prefix.h neutralises.
 *
 * The same header serves the layout probe, compiled as 32-bit freestanding code (HOST_PROBE): there only the
 * compiler's own headers exist, and the types come out with the GameCube/3DS layout. */
#ifndef HOST_PREFIX_H
#define HOST_PREFIX_H

#define TARGET_PORT 1
#define TARGET_HOST 1

#define __declspec(...)
#define PORT_ENUM_PACKED __attribute__((packed))

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#ifndef HOST_PROBE
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#endif

#define _DOLPHIN_TYPES_H_ /* keep dolphin/types.h out: the types are below */
typedef int8_t s8;
typedef uint8_t u8;
typedef int16_t s16;
typedef uint16_t u16;
typedef int32_t s32;
typedef uint32_t u32;
typedef signed long long s64; /* as dolphin/types.h: 64 bits wide, spelled long long */
typedef unsigned long long u64;
typedef volatile int8_t vs8;
typedef volatile uint8_t vu8;
typedef volatile int16_t vs16;
typedef volatile uint16_t vu16;
typedef volatile int32_t vs32;
typedef volatile uint32_t vu32;
typedef volatile signed long long vs64;
typedef volatile unsigned long long vu64;
typedef float f32;
typedef double f64;
typedef char* Ptr;
typedef int BOOL;
#define FALSE 0
#define TRUE 1
#define AT_ADDRESS(addr)
#define ATTRIBUTE_ALIGN(num) __attribute__((aligned(num)))
#define INT_MAX 2147483647
#ifndef NULL
#define NULL ((void*)0)
#endif

#endif
