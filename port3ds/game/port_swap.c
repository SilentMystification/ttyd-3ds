/* In-place conversion of big-endian disc data: see port3ds/port_swap.h. */
#include <stdlib.h>
#include <string.h>

#include "plat/plat.h"
#include "port3ds/port_swap.h"

void port_swap_begin(PortSwap* s, void* base, u32 size, const char* what) {
    s->base = base;
    s->size = size;
    s->what = what != NULL ? what : "?";
    s->done = calloc(size / 8 + 1, 1);
    s->visited = calloc(size / 8 + 1, 1);
    if (s->done == NULL || s->visited == NULL) {
        plat_fatal("%s: no memory to convert %lu bytes", s->what, (unsigned long)size);
    }
}

void port_swap_end(PortSwap* s) {
    free(s->done);
    free(s->visited);
    s->done = NULL;
    s->visited = NULL;
}

int port_swap_in(const PortSwap* s, u32 off, u32 len) { return off <= s->size && len <= s->size - off; }

void port_swap_bad(const PortSwap* s, const char* field, u32 off, u32 len) {
    plat_fatal("%s: %s at %#lx (+%#lx) is outside the file (%#lx bytes)", s->what, field, (unsigned long)off,
               (unsigned long)len, (unsigned long)s->size);
}

static inline int is_done(const PortSwap* s, u32 off) { return s->done[off >> 3] >> (off & 7) & 1; }

static inline void mark(PortSwap* s, u32 off) { s->done[off >> 3] |= (u8)(1u << (off & 7)); }

int port_swap_visit(PortSwap* s, u32 off) {
    if (off >= s->size) {
        port_swap_bad(s, "node", off, 1);
    }
    if (s->visited[off >> 3] >> (off & 7) & 1) {
        return 0;
    }
    s->visited[off >> 3] |= (u8)(1u << (off & 7));
    return 1;
}

static void swap_unit(PortSwap* s, u32 off, u32 width) {
    if (is_done(s, off)) {
        return;
    }
    u8* p = s->base + off;
    u8 t;
    switch (width) {
    case 2:
        t = p[0], p[0] = p[1], p[1] = t;
        break;
    case 4:
        t = p[0], p[0] = p[3], p[3] = t;
        t = p[1], p[1] = p[2], p[2] = t;
        break;
    case 8:
        for (int i = 0; i < 4; i++) {
            t = p[i], p[i] = p[7 - i], p[7 - i] = t;
        }
        break;
    }
    for (u32 i = 0; i < width; i++) {
        mark(s, off + i);
    }
}

void port_swap_word_any(PortSwap* s, u32 off) {
    if (!port_swap_in(s, off, 4)) {
        port_swap_bad(s, "word", off, 4);
    }
    swap_unit(s, off, 4);
}

void port_swap_units(PortSwap* s, u32 off, u32 n, u32 width) {
    if ((off & 1) != 0 || (width != 2 && width != 4 && width != 8)) {
        plat_fatal("%s: unaligned or bad unit (%#lx, width %lu)", s->what, (unsigned long)off, (unsigned long)width);
    }
    if (n > s->size / width || !port_swap_in(s, off, n * width)) {
        port_swap_bad(s, "data", off, n * width);
    }
    for (u32 i = 0; i < n; i++) {
        swap_unit(s, off + i * width, width);
    }
}

void port_swap_struct(PortSwap* s, u32 off, const PortSwapType* t) {
    if (!port_swap_in(s, off, t->size)) {
        port_swap_bad(s, t->name, off, t->size);
    }
    for (u32 r = 0; r < t->nruns; r++) {
        const PortSwapRun* run = &t->runs[r];
        for (u32 i = 0; i < run->count; i++) {
            swap_unit(s, off + run->offset + i * run->width, run->width);
        }
    }
}

void port_swap_array(PortSwap* s, u32 off, u32 n, const PortSwapType* t) {
    if (n != 0 && (n > s->size / t->size || !port_swap_in(s, off, n * t->size))) {
        port_swap_bad(s, t->name, off, n * t->size);
    }
    for (u32 i = 0; i < n; i++) {
        port_swap_struct(s, off + i * t->size, t);
    }
}

u16 port_swap_get16(const PortSwap* s, u32 off) {
    if (!port_swap_in(s, off, 2)) {
        port_swap_bad(s, "u16", off, 2);
    }
    u16 v;
    memcpy(&v, s->base + off, 2);
    return is_done(s, off) ? v : port_be16(s->base + off);
}

u32 port_swap_get32(const PortSwap* s, u32 off) {
    if (!port_swap_in(s, off, 4)) {
        port_swap_bad(s, "u32", off, 4);
    }
    u32 v;
    memcpy(&v, s->base + off, 4);
    return is_done(s, off) ? v : port_be32(s->base + off);
}

void portSwapU16Run(void* data, u32 count) {
    u8* p = data;
    for (u32 i = 0; i < count; i++, p += 2) {
        u8 t = p[0];
        p[0] = p[1];
        p[1] = t;
    }
}

void portSwapU32Run(void* data, u32 count) {
    u8* p = data;
    for (u32 i = 0; i < count; i++, p += 4) {
        u8 t = p[0];
        p[0] = p[3];
        p[3] = t;
        t = p[1];
        p[1] = p[2];
        p[2] = t;
    }
}
