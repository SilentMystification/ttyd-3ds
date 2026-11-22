/* In-place conversion of big-endian disc data (port3ds/game/port_swap.c).
 *
 * A PortSwap covers one loaded file (or one self-contained part of it). The
 * format loaders (port3ds/game/formats/) walk the file's pointer graph and
 * swap each struct they reach with its generated table (port_swap_gen.h,
 * tools/port3ds/gen_swap.py). A bitmap remembers which bytes are done, so a struct reached twice (shared nodes, overlapping lists) is
 * swapped once. Every access is checked against the file's size; a bad
 * offset stops the game with the file's name (a port bug or a damaged
 * image, never something to run on with). */
#ifndef PORT3DS_PORT_SWAP_H
#define PORT3DS_PORT_SWAP_H

#include <dolphin/types.h>

typedef struct {
    u32 offset;
    u8 width; /* 2, 4 or 8 */
    u32 count;
} PortSwapRun;

typedef struct {
    const char* name;
    u32 size;
    u32 nruns;
    const PortSwapRun* runs;
} PortSwapType;

typedef struct {
    u8* base;
    u32 size;
    u8* done;    /* one bit per byte: converted */
    u8* visited; /* one bit per byte: a node walked from here (port_swap_visit) */
    const char* what;
} PortSwap;

/* Size for files whose length the caller does not know (UnpackTexPalette):
 * offsets are still checked against it. */
#define PORT_SWAP_SIZE_UNKNOWN 0x01000000u

void port_swap_begin(PortSwap* s, void* base, u32 size, const char* what);
void port_swap_end(PortSwap* s);

/* 1 when [off, off + len) lies inside the file. */
int port_swap_in(const PortSwap* s, u32 off, u32 len);
/* Stops the game: offset `off` (+len) is outside the file. */
void port_swap_bad(const PortSwap* s, const char* field, u32 off, u32 len) __attribute__((noreturn));

/* Swaps one struct, or n of them in a row, at file offset off. */
void port_swap_struct(PortSwap* s, u32 off, const PortSwapType* t);
void port_swap_array(PortSwap* s, u32 off, u32 n, const PortSwapType* t);
/* Swaps n units of 2, 4 or 8 bytes at off (even). */
void port_swap_units(PortSwap* s, u32 off, u32 n, u32 width);
/* Swaps one 4-byte word at any alignment (relocation entries need not be
 * aligned). */
void port_swap_word_any(PortSwap* s, u32 off);
/* 1 the first time a node at off is reached, 0 after: walks over graphs
 * with cycles and shared nodes. */
int port_swap_visit(PortSwap* s, u32 off);

/* The value of a field after its swap (host order). */
u16 port_swap_get16(const PortSwap* s, u32 off);
u32 port_swap_get32(const PortSwap* s, u32 off);

/* Load-time conversion profile (switch loadstats). Each converter brackets
 * its work with port_conv_begin/port_conv_end; with the switch, every
 * conversion logs a [LOADCONV] line (kind, file, bytes, time) and every load
 * (a burst of conversions; a gap of a second ends one) logs a summary per
 * kind with the DVD reads in between. Switch convhash adds a hash of each
 * converted file, to compare runs (jobs=0 against jobs). */
enum {
    PORT_CONV_MAP,
    PORT_CONV_TPL,
    PORT_CONV_ANIM,
    PORT_CONV_FONT,
    PORT_CONV_CAMROAD,
    PORT_CONV_MUSYX,
    PORT_CONV_WORDS, /* portSwapU16Run / portSwapU32Run */
    PORT_CONV_KINDS
};
u64 port_conv_begin(void);
void port_conv_end(int kind, u64 start, const char* name, const void* data, u32 bytes);

/* Big-endian reads of raw bytes. */
static inline u16 port_be16(const void* p) {
    const u8* b = (const u8*)p;
    return (u16)(b[0] << 8 | b[1]);
}
static inline u32 port_be32(const void* p) {
    const u8* b = (const u8*)p;
    return (u32)b[0] << 24 | (u32)b[1] << 16 | (u32)b[2] << 8 | b[3];
}

#endif
