/* In-place conversion of big-endian disc data: see port3ds/port_swap.h. */
#include <stdio.h>
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

/* ---- Load-time conversion profile (switch loadstats) ---- */

void port_dvd_read_stats(u32* reads, u64* bytes, u64* ticks);

static const char* const k_conv_kind[PORT_CONV_KINDS] = {"map", "tpl", "anim", "font", "camroad", "musyx", "words"};
#define BURST_GAP_TICKS ((u64)PLAT_TICKS_PER_SEC) /* a second without conversions ends a load */

typedef struct {
    u32 count;
    u32 bytes;
    u64 ticks;
} ConvKind;

static int s_conv_on = -1; /* -1 not read yet */
static int s_conv_hash;
static u32 s_conv_depth; /* nested conversions (a .slib's songs) count once */
static ConvKind s_burst[PORT_CONV_KINDS];
static u64 s_burst_start, s_burst_last;
static u32 s_burst_n;
static u32 s_burst_dvd_reads;
static u64 s_burst_dvd_bytes, s_burst_dvd_ticks;

static u32 ticks_us(u64 t) { return (u32)(t / (PLAT_TICKS_PER_SEC / 1000000u)); }

static void burst_flush(void) {
    if (s_burst_n == 0) {
        return;
    }
    u32 reads;
    u64 bytes, ticks;
    port_dvd_read_stats(&reads, &bytes, &ticks);
    u64 conv = 0;
    u32 conv_bytes = 0;
    char line[320];
    int n = 0;
    for (int k = 0; k < PORT_CONV_KINDS; k++) {
        conv += s_burst[k].ticks;
        conv_bytes += s_burst[k].bytes;
        if (s_burst[k].count != 0 && n < (int)sizeof(line)) {
            n += snprintf(line + n, sizeof(line) - n, " %s %lu/%luB/%luus", k_conv_kind[k],
                          (unsigned long)s_burst[k].count, (unsigned long)s_burst[k].bytes,
                          (unsigned long)ticks_us(s_burst[k].ticks));
        }
    }
    PLOG_I("[LOADCONV] load: %lu files, %lu bytes, convert %lu us of %lu us; dvd %lu reads %llu bytes %lu us;%s",
           (unsigned long)s_burst_n, (unsigned long)conv_bytes, (unsigned long)ticks_us(conv),
           (unsigned long)ticks_us(s_burst_last - s_burst_start), (unsigned long)(reads - s_burst_dvd_reads),
           (unsigned long long)(bytes - s_burst_dvd_bytes), (unsigned long)ticks_us(ticks - s_burst_dvd_ticks), line);
    memset(s_burst, 0, sizeof(s_burst));
    s_burst_n = 0;
}

u64 port_conv_begin(void) {
    if (s_conv_on < 0) {
        s_conv_on = plat_switch_has("loadstats");
        s_conv_hash = plat_switch_has("convhash");
    }
    if (!s_conv_on || s_conv_depth++ != 0) {
        return 0;
    }
    u64 now = plat_ticks();
    if (s_burst_n != 0 && now - s_burst_last > BURST_GAP_TICKS) {
        burst_flush();
    }
    if (s_burst_n == 0) {
        s_burst_start = now;
        port_dvd_read_stats(&s_burst_dvd_reads, &s_burst_dvd_bytes, &s_burst_dvd_ticks);
    }
    return now;
}

/* FNV-1a over the converted bytes. */
static u32 conv_hash(const void* data, u32 bytes) {
    const u8* p = data;
    u32 h = 2166136261u;
    for (u32 i = 0; i < bytes; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

void port_conv_end(int kind, u64 start, const char* name, const void* data, u32 bytes) {
    if (!s_conv_on || --s_conv_depth != 0) {
        return;
    }
    u64 now = plat_ticks();
    ConvKind* k = &s_burst[kind];
    k->count++;
    k->bytes += bytes;
    k->ticks += now - start;
    s_burst_n++;
    s_burst_last = now;
    if (s_conv_hash && data != NULL) {
        PLOG_I("[LOADCONV] %s %s: %lu bytes, %lu us, hash %08lx", k_conv_kind[kind], name ? name : "?",
               (unsigned long)bytes, (unsigned long)ticks_us(now - start), (unsigned long)conv_hash(data, bytes));
        s_burst_last = plat_ticks(); /* the hash is not part of the load */
    } else {
        PLOG_I("[LOADCONV] %s %s: %lu bytes, %lu us", k_conv_kind[kind], name ? name : "?", (unsigned long)bytes,
               (unsigned long)ticks_us(now - start));
    }
}

void portSwapU16Run(void* data, u32 count) {
    u64 t0 = port_conv_begin();
    u8* p = data;
    for (u32 i = 0; i < count; i++, p += 2) {
        u8 t = p[0];
        p[0] = p[1];
        p[1] = t;
    }
    port_conv_end(PORT_CONV_WORDS, t0, "u16 run", data, count * 2);
}

void portSwapU32Run(void* data, u32 count) {
    u64 t0 = port_conv_begin();
    u8* p = data;
    for (u32 i = 0; i < count; i++, p += 4) {
        u8 t = p[0];
        p[0] = p[3];
        p[3] = t;
        t = p[1];
        p[1] = p[2];
        p[2] = t;
    }
    port_conv_end(PORT_CONV_WORDS, t0, "u32 run", data, count * 4);
}
