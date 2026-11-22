/* Content-stable memory registry (include/plat/memstable.h).
 *
 * The covered memory is a sorted table of disjoint ranges [start, end). A
 * range is "relied" once a consumer was given trust over it; only a report
 * that touches a relied range bumps the trust generation, so staging buffers
 * the disc reads into over and over cost nothing. Every report that touches
 * covered memory bumps the epoch, which makes a trust request that raced
 * with it fail (the consumer asks again later).
 *
 * Adjacent ranges that are not relied are merged (a file read in chunks is
 * one range). A full table drops the new fill: that memory stays uncovered,
 * which only costs speed. */
#include <3ds.h>
#include <string.h>

#include "plat/memstable.h"
#include "plat/plat.h"

#define MAX_RANGES 8192

typedef struct {
    uintptr_t start, end;
    uint32_t relied;
} Range;

static Range s_r[MAX_RANGES];
static uint32_t s_n;
static LightLock s_lock = 1; /* the value LightLock_Init sets: unlocked */
static uint32_t s_gen = 1, s_epoch = 1;
static uint32_t s_fills, s_writes, s_releases, s_revokes, s_dropped;

uint32_t plat_memstable_epoch(void) { return __atomic_load_n(&s_epoch, __ATOMIC_ACQUIRE); }
uint32_t plat_memstable_gen(void) { return __atomic_load_n(&s_gen, __ATOMIC_ACQUIRE); }

static void span_of(const void* p, uint32_t n, uintptr_t* a, uintptr_t* b) {
    *a = (uintptr_t)p;
    *b = *a + n;
    if (*b < *a) {
        *b = UINTPTR_MAX;
    }
}

/* First range with end > a. */
static uint32_t lower(uintptr_t a) {
    uint32_t lo = 0, hi = s_n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (s_r[mid].end <= a) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/* Something covered changed: the epoch always, the generation when a consumer relied on it. */
static void changed(int relied) {
    __atomic_store_n(&s_epoch, s_epoch + 1, __ATOMIC_RELEASE);
    if (relied) {
        uint32_t g = s_gen + 1;
        __atomic_store_n(&s_gen, g != 0 ? g : 1, __ATOMIC_RELEASE);
        s_revokes++;
    }
}

/* Whether [a, b) overlaps covered memory: 0 no, 1 yes, 3 yes and some of it relied. */
static int overlap(uintptr_t a, uintptr_t b) {
    int r = 0;
    for (uint32_t i = lower(a); i < s_n && s_r[i].start < b; i++) {
        r |= 1 | (s_r[i].relied ? 2 : 0);
    }
    return r;
}

/* Removes [a, b) from the coverage. */
static void cut(uintptr_t a, uintptr_t b) {
    uint32_t i = lower(a), j = i;
    while (j < s_n && s_r[j].start < b) {
        j++;
    }
    if (i == j) {
        return;
    }
    Range left = s_r[i], right = s_r[j - 1];
    Range keep[2];
    uint32_t k = 0;
    if (left.start < a) {
        left.end = a;
        keep[k++] = left;
    }
    if (right.end > b) {
        right.start = b;
        keep[k++] = right;
    }
    if (s_n - (j - i) + k > MAX_RANGES) {
        k--; /* the table is full: the right part goes uncovered */
    }
    memmove(&s_r[i + k], &s_r[j], (s_n - j) * sizeof(Range));
    memcpy(&s_r[i], keep, k * sizeof(Range));
    s_n = s_n - (j - i) + k;
}

void plat_memstable_fill(const void* p, uint32_t n) {
    uintptr_t a, b;
    if (n == 0) {
        return;
    }
    span_of(p, n, &a, &b);
    LightLock_Lock(&s_lock);
    s_fills++;
    int o = overlap(a, b);
    if (o) {
        changed(o & 2);
        cut(a, b);
    }
    uint32_t i = lower(a); /* nothing overlaps [a, b) now */
    int merge_l = i > 0 && s_r[i - 1].end == a && !s_r[i - 1].relied;
    int merge_r = i < s_n && s_r[i].start == b && !s_r[i].relied;
    if (merge_l && merge_r) {
        s_r[i - 1].end = s_r[i].end;
        memmove(&s_r[i], &s_r[i + 1], (s_n - i - 1) * sizeof(Range));
        s_n--;
    } else if (merge_l) {
        s_r[i - 1].end = b;
    } else if (merge_r) {
        s_r[i].start = a;
    } else if (s_n < MAX_RANGES) {
        memmove(&s_r[i + 1], &s_r[i], (s_n - i) * sizeof(Range));
        s_r[i] = (Range){a, b, 0};
        s_n++;
    } else {
        s_dropped++;
    }
    LightLock_Unlock(&s_lock);
}

void plat_memstable_write(const void* p, uint32_t n) {
    uintptr_t a, b;
    if (n == 0 || __atomic_load_n(&s_n, __ATOMIC_RELAXED) == 0) {
        return; /* nothing covered: no trust can depend on it (a later fill bumps the epoch) */
    }
    span_of(p, n, &a, &b);
    LightLock_Lock(&s_lock);
    s_writes++;
    int o = overlap(a, b);
    if (o) {
        changed(o & 2);
    }
    LightLock_Unlock(&s_lock);
}

void plat_memstable_release(const void* p, uint32_t n) {
    uintptr_t a, b;
    if (n == 0 || __atomic_load_n(&s_n, __ATOMIC_RELAXED) == 0) {
        return;
    }
    span_of(p, n, &a, &b);
    LightLock_Lock(&s_lock);
    s_releases++;
    int o = overlap(a, b);
    if (o) {
        changed(o & 2);
        cut(a, b);
    }
    LightLock_Unlock(&s_lock);
}

/* The ranges covering [a, b) without a gap: first index, or -1. *last gets the last index. */
static int32_t covering(uintptr_t a, uintptr_t b, uint32_t* last) {
    uint32_t i = lower(a);
    if (i >= s_n || s_r[i].start > a) {
        return -1;
    }
    uint32_t j = i;
    while (s_r[j].end < b) {
        if (j + 1 >= s_n || s_r[j + 1].start != s_r[j].end) {
            return -1;
        }
        j++;
    }
    *last = j;
    return (int32_t)i;
}

uint32_t plat_memstable_trust(const PlatMemSpan* spans, uint32_t count, uint32_t epoch) {
    uint32_t g = 0;
    LightLock_Lock(&s_lock);
    if (s_epoch == epoch) {
        uint32_t k;
        for (k = 0; k < count; k++) {
            uintptr_t a, b;
            uint32_t last;
            span_of(spans[k].p, spans[k].n, &a, &b);
            if (spans[k].n != 0 && covering(a, b, &last) < 0) {
                break;
            }
        }
        if (k == count) {
            for (k = 0; k < count; k++) {
                uintptr_t a, b;
                uint32_t last;
                span_of(spans[k].p, spans[k].n, &a, &b);
                int32_t i = spans[k].n != 0 ? covering(a, b, &last) : -1;
                for (int32_t j = i; i >= 0 && j <= (int32_t)last; j++) {
                    s_r[j].relied = 1;
                }
            }
            g = s_gen;
        }
    }
    LightLock_Unlock(&s_lock);
    return g;
}

void plat_memstable_stats(PlatMemStableStats* out) {
    LightLock_Lock(&s_lock);
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < s_n; i++) {
        bytes += s_r[i].end - s_r[i].start;
    }
    out->ranges = s_n;
    out->covered_kb = (uint32_t)(bytes >> 10);
    out->fills = s_fills;
    out->writes = s_writes;
    out->releases = s_releases;
    out->revokes = s_revokes;
    out->dropped = s_dropped;
    s_fills = s_writes = s_releases = s_revokes = 0;
    LightLock_Unlock(&s_lock);
}
