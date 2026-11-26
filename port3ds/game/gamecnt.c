/* Step 0 counters (switch gamecnt=N, jobprobe=1; port3ds/include/port3ds/gamepar.h).
 *
 * Every N game frames, [GAMECNT] lines with per-frame means since the last report. Callers are return
 * addresses (resolve with addr2line on the ELF of the same build; with LTO a caller that inlined the lookup
 * shows as its own caller). All counters run on the game thread. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "plat/job.h"
#include "plat/plat.h"
#include "port3ds/gamepar.h"

int port_gamecnt = -1;
static int s_jobprobe;

#define NCALLER 32u /* power of two */

typedef struct {
    void* ra;
    u32 calls;
    u64 sum; /* probes (lookups) or elements (qqsort) */
} Caller;

typedef struct {
    u32 calls;
    u64 probes, compares;
    u32 lost; /* callers that did not fit the table */
    Caller tab[NCALLER];
} LookupCnt;

static LookupCnt s_look[3];
static LookupCnt s_sort; /* probes = elements, compares unused */
static u32 s_sort_max;
static u32 s_hitvec, s_anim, s_mapdisp;
static u64 s_mapobj, s_mapent;
static u32 s_from;

typedef struct {
    u32 n, fails;
    u64 sum, min, max;
} Probe;
static Probe s_probe[2];

int port_gamecnt_probe(void) {
    int n = plat_switch_int("gamecnt", 0);
    port_gamecnt = n > 0 ? n : 0;
    s_jobprobe = plat_switch_int("jobprobe", 0);
    if (port_gamecnt > 0) {
        PLOG_I("gamecnt: counters every %d frames%s", port_gamecnt, s_jobprobe ? ", job round trips" : "");
    }
    return port_gamecnt;
}

static void caller_add(LookupCnt* c, void* ra, u32 v) {
    u32 h = ((u32)(uintptr_t)ra >> 2) * 2654435761u >> 27; /* 5 bits */
    for (u32 k = 0; k < NCALLER; k++) {
        Caller* e = &c->tab[(h + k) & (NCALLER - 1)];
        if (e->ra == ra) {
            e->calls++;
            e->sum += v;
            return;
        }
        if (e->ra == NULL) {
            e->ra = ra;
            e->calls = 1;
            e->sum = v;
            return;
        }
    }
    c->lost++;
}

void port_cnt_lookup(int kind, void* caller, u32 probes, u32 compares) {
    LookupCnt* c = &s_look[kind];
    c->calls++;
    c->probes += probes;
    c->compares += compares;
    caller_add(c, caller, probes);
}

void port_cnt_qqsort(void* caller, u32 n) {
    s_sort.calls++;
    s_sort.probes += n;
    if (n > s_sort_max) {
        s_sort_max = n;
    }
    caller_add(&s_sort, caller, n);
}

void port_cnt_hitvec(void) { s_hitvec++; }
void port_cnt_animpose(void) { s_anim++; }

static u64 s_mapdisp_ticks, s_lookup_ticks;

void port_cnt_mapdisp(u32 objects, u32 entries, u64 ticks) {
    s_mapdisp++;
    s_mapobj += objects;
    s_mapent += entries;
    s_mapdisp_ticks += ticks;
}

void port_cnt_lookup_ticks(u64 ticks) { s_lookup_ticks += ticks; }

/* x / d with one decimal, as "12.3" */
static const char* dec1(char* buf, u64 x, u32 d) {
    if (d == 0) {
        d = 1;
    }
    u64 t = (x * 10u + d / 2) / d;
    snprintf(buf, 16, "%lu.%lu", (unsigned long)(t / 10u), (unsigned long)(t % 10u));
    return buf;
}

static void top_line(const char* what, LookupCnt* c, u32 frames) {
    char line[320];
    int n = 0;
    char a[16], b[16];
    for (int r = 0; r < 6; r++) {
        Caller* best = NULL;
        for (u32 k = 0; k < NCALLER; k++) {
            Caller* e = &c->tab[k];
            if (e->ra != NULL && e->calls != 0 && (best == NULL || e->calls > best->calls)) {
                best = e;
            }
        }
        if (best == NULL || n > (int)sizeof(line) - 48) {
            break;
        }
        n += snprintf(line + n, sizeof(line) - n, " %p:%s/f(%s)", best->ra, dec1(a, best->calls, frames), dec1(b, best->sum, best->calls));
        best->calls = 0; /* taken; the table is cleared after the report */
    }
    PLOG_I("[GAMECNT]   %s top callers (calls per frame, mean %s):%s%s", what, c == &s_sort ? "n" : "probes", n ? line : " none",
           c->lost ? " (some callers not kept)" : "");
}

static void probe_jobs(void) {
    for (int w = 0; w < 2; w++) {
        u64 t;
        int r = plat_job_probe(w, &t);
        if (r < 0) {
            continue; /* no such worker */
        }
        Probe* p = &s_probe[w];
        if (r == 0) {
            p->fails++;
            continue;
        }
        if (p->n == 0 || t < p->min) {
            p->min = t;
        }
        if (t > p->max) {
            p->max = t;
        }
        p->sum += t;
        p->n++;
    }
}

void port_gamepar_frame(u32 frame) {
    port_nameidx_frame(frame);
    portMapDispJobFrame(frame);
    if (!port_gamecnt_on()) {
        return;
    }
    if (s_jobprobe) {
        probe_jobs();
    }
    if (s_from == 0) {
        s_from = frame;
        return;
    }
    u32 frames = frame - s_from;
    if (frames < (u32)port_gamecnt) {
        return;
    }
    char a[16], b[16], c[16], d[16], e[16], f[16], g[16];
    PLOG_I("[GAMECNT] frame %lu (%lu frames, per frame): mapDisp %s calls %s objects walked %s dispEntry | hitCheckVecFilter %s | "
           "animPoseMain %s | qqsort %s calls, n mean %s max %lu",
           (unsigned long)frame, (unsigned long)frames, dec1(a, s_mapdisp, frames), dec1(b, s_mapobj, frames), dec1(c, s_mapent, frames),
           dec1(d, s_hitvec, frames), dec1(e, s_anim, frames), dec1(f, s_sort.calls, frames), dec1(g, s_sort.probes, s_sort.calls),
           (unsigned long)s_sort_max);
    {
        const u64 tpu = PLAT_TICKS_PER_SEC / 1000000u;
        PLOG_I("[GAMECNT]   time per frame: mapDisp %lu us, name lookups %lu us (both include the counters)",
               (unsigned long)(s_mapdisp_ticks / tpu / frames), (unsigned long)(s_lookup_ticks / tpu / frames));
        s_mapdisp_ticks = s_lookup_ticks = 0;
    }
    static const char* const k_kind[3] = {"mapGetMapObj", "hitNameToPtr", "lightNameToPtr"};
    for (int k = 0; k < 3; k++) {
        LookupCnt* l = &s_look[k];
        PLOG_I("[GAMECNT]   %s: %s calls, %s probes and %s strcmp per call", k_kind[k], dec1(a, l->calls, frames),
               dec1(b, l->probes, l->calls), dec1(c, l->compares, l->calls));
        if (l->calls != 0) {
            top_line(k_kind[k], l, frames);
        }
    }
    if (s_sort.calls != 0) {
        top_line("qqsort", &s_sort, frames);
    }
    if (s_jobprobe) {
        const u64 tpu = PLAT_TICKS_PER_SEC / 1000000u;
        for (int w = 0; w < 2; w++) {
            Probe* p = &s_probe[w];
            if (p->n == 0 && p->fails == 0) {
                continue;
            }
            PLOG_I("[GAMECNT]   job round trip, worker %d: %lu probes, min %lu us mean %lu us max %lu us, %lu not run within 5 ms", w,
                   (unsigned long)p->n, (unsigned long)(p->min / tpu), (unsigned long)(p->n ? p->sum / p->n / tpu : 0),
                   (unsigned long)(p->max / tpu), (unsigned long)p->fails);
        }
        memset(s_probe, 0, sizeof(s_probe));
    }
    memset(s_look, 0, sizeof(s_look));
    memset(&s_sort, 0, sizeof(s_sort));
    s_sort_max = 0;
    s_hitvec = s_anim = s_mapdisp = 0;
    s_mapobj = s_mapent = 0;
    s_from = frame;
}
