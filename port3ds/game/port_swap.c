/* In-place conversion of big-endian disc data: see port3ds/port_swap.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plat/job.h"
#include "plat/plat.h"
#include "port3ds/loadprof.h"
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

static u32 s_check_runs, s_check_bad; /* jobcheck: split runs compared, and those that differed */
static u32 s_check_runs_logged, s_check_bad_logged;

void port_swap_end(PortSwap* s) {
    if (s_check_runs != s_check_runs_logged) {
        PLOG_I("[JOBCHECK] %s: %lu split runs, %lu identical to inline", s->what,
               (unsigned long)(s_check_runs - s_check_runs_logged),
               (unsigned long)((s_check_runs - s_check_runs_logged) - (s_check_bad - s_check_bad_logged)));
        s_check_runs_logged = s_check_runs;
        s_check_bad_logged = s_check_bad;
    }
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

/* ---- Long runs on the job worker ----
 *
 * A run of units or structs splits cleanly: each item's swap depends only
 * on its own bytes and their bits in the done bitmap. Chunk edges fall on
 * 8-byte file offsets, so two chunks never share a bitmap byte, and the
 * result is the same in any order. Switch jobcheck=1 runs each split run
 * twice (jobs, then inline on the original bytes) and compares. */

#define SPLIT_MIN_BYTES (16u * 1024u) /* per chunk; shorter runs stay inline */

typedef struct {
    PortSwap* s;
    u32 off;    /* file offset of item 0 */
    u32 stride; /* bytes per item */
    u32 width;  /* units: unit width */
    const PortSwapType* t; /* structs: the type */
} SplitRun;

typedef void (*SplitFn)(void* arg, uint32_t begin, uint32_t end);

static void units_range(void* arg, uint32_t b, uint32_t e) {
    const SplitRun* r = arg;
    for (u32 i = b; i < e; i++) {
        swap_unit(r->s, r->off + i * r->stride, r->width);
    }
}

static void structs_range(void* arg, uint32_t b, uint32_t e) {
    const SplitRun* r = arg;
    for (u32 i = b; i < e; i++) {
        port_swap_struct(r->s, r->off + i * r->stride, r->t);
    }
}

static int s_jobcheck = -1;

static void split_run(SplitFn fn, SplitRun* r, u32 n, int jobs) {
    if (jobs) {
        u32 align = 8u >> __builtin_ctz(r->stride | 8u); /* items per 8 bytes */
        u32 min_items = SPLIT_MIN_BYTES / r->stride;
        plat_job_parallel_for(fn, r, n, min_items ? min_items : 1, align);
    } else {
        fn(r, 0, n);
    }
}

static void split(PortSwap* s, SplitFn fn, u32 off, u32 n, u32 stride, u32 width, const PortSwapType* t) {
    SplitRun r = {s, off, stride, width, t};
    if ((u64)n * stride < 2u * SPLIT_MIN_BYTES || !plat_job_enabled()) {
        fn(&r, 0, n);
        return;
    }
    /* The first item that starts on an 8-byte offset; the ones before run here. */
    u32 first = 0;
    while (first < 8 && first < n && ((off + first * stride) & 7u) != 0) {
        first++;
    }
    if (first == 8 || first == n) {
        fn(&r, 0, n); /* no item on an 8-byte edge (an odd 2-byte phase): stays inline */
        return;
    }
    fn(&r, 0, first);
    r.off = off + first * stride;
    n -= first;
    if (s_jobcheck < 0) {
        s_jobcheck = plat_switch_int("jobcheck", 0) != 0;
    }
    if (!s_jobcheck) {
        split_run(fn, &r, n, 1);
        return;
    }
    /* Check mode: the same run with jobs and inline, from the same start. */
    u32 len = n * stride, b0 = r.off >> 3, b1 = (r.off + len + 7) >> 3;
    u8* data0 = malloc(len);
    u8* done0 = malloc(b1 - b0);
    u8* data1 = malloc(len);
    u8* done1 = malloc(b1 - b0);
    if (data0 == NULL || done0 == NULL || data1 == NULL || done1 == NULL) {
        plat_fatal("jobcheck: no memory for %lu bytes", (unsigned long)len);
    }
    memcpy(data0, s->base + r.off, len);
    memcpy(done0, s->done + b0, b1 - b0);
    split_run(fn, &r, n, 1);
    memcpy(data1, s->base + r.off, len);
    memcpy(done1, s->done + b0, b1 - b0);
    memcpy(s->base + r.off, data0, len);
    memcpy(s->done + b0, done0, b1 - b0);
    split_run(fn, &r, n, 0);
    s_check_runs++;
    if (memcmp(data1, s->base + r.off, len) != 0 || memcmp(done1, s->done + b0, b1 - b0) != 0) {
        s_check_bad++;
        PLOG_E("[JOBCHECK] %s: run at %#lx (%lu items of %lu bytes) differs between jobs and inline", s->what,
               (unsigned long)r.off, (unsigned long)n, (unsigned long)stride);
    }
    free(data0);
    free(done0);
    free(data1);
    free(done1);
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
    split(s, units_range, off, n, width, width, NULL);
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
    split(s, structs_range, off, n, t->size, 0, t);
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
        s_conv_on = plat_switch_has("loadstats") || port_lp_active();
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
    port_lp_conv(now - start);
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
