/* Map-change and battle-start load profile (switch loadprof; port3ds/include/port3ds/loadprof.h). */
#include <stdio.h>
#include <string.h>

#include "driver/seqdrv.h"
#include "plat/disc.h"
#include "plat/plat.h"
#include "port3ds/loadprof.h"
#include "version.h"

int port_lp_state = -1;

#define HIST 7
static const char* const k_hist_name[HIST] = {"<=4K", "<=16K", "<=64K", "<=256K", "<=1M", ">1M", "all"};

typedef struct {
    u32 count, seq_count;
    u64 bytes, ticks, max_ticks;
} Hist;

typedef struct {
    Hist dvd[HIST];
    u64 wake, fs_bytes;
    u32 fs_calls, ra_hits, merged;
    u32 reads_per_frame[4]; /* frames that ended 0, 1, 2, 3+ reads after the last */
    u32 aram_posts;
    u64 aram_bytes, aram_sim, aram_slept, spin, conv, retrace, sd;
    u32 conv_n;
} Cnt;

static Cnt s_c; /* cumulative */
static u64 s_dvd_end;

#define POST_FRAMES 90
#define POST_SUM 30

static int s_state; /* 0 idle, 1 window, 2 post */
static int s_kind;  /* 0 map, 1 battle */
static Cnt s_start;
static u64 s_t0;
static u32 s_f0, s_nframes;
static u64 s_busy_sum;
static char s_name[16];
static int s_prev_seq = -1;
static u32 s_post_n;
static u64 s_post_busy[POST_FRAMES + 1];
static Cnt s_post_start;
static u32 s_post_end_frame;

static u32 us(u64 t) { return (u32)(t / (PLAT_TICKS_PER_SEC / 1000000u)); }
static u32 ms10(u64 t) { return (u32)(t / (PLAT_TICKS_PER_SEC / 100000u)); } /* 0.01 ms */

int port_lp_probe(void) {
    port_lp_state = plat_switch_has("loadprof") ? 1 : 0;
    return port_lp_state;
}

static int bucket(u32 size) {
    if (size <= 4096) return 0;
    if (size <= 16384) return 1;
    if (size <= 65536) return 2;
    if (size <= 262144) return 3;
    if (size <= 1048576) return 4;
    return 5;
}

/* The hooks run on one thread each except dvd (under the interrupt lock); counters are diagnostics. */
static u64 s_dvd_done_tick;
void port_lp_dvd_done(u64 tick) { s_dvd_done_tick = tick; }

void port_lp_dvd(u32 off, u32 size, u64 ticks) {
    u64 now = plat_ticks();
    s_c.wake += now >= s_dvd_done_tick ? now - s_dvd_done_tick : 0;
    int seq = off == s_dvd_end;
    s_dvd_end = (u64)off + size;
    for (int i = 0; i < 2; i++) {
        Hist* h = &s_c.dvd[i == 0 ? bucket(size) : HIST - 1];
        h->count++;
        h->seq_count += seq;
        h->bytes += size;
        h->ticks += ticks;
        if (ticks > h->max_ticks) {
            h->max_ticks = ticks;
        }
    }
}

void port_lp_aram(u32 bytes, u64 sim, u64 slept) {
    s_c.aram_posts++;
    s_c.aram_bytes += bytes;
    s_c.aram_sim += sim;
    s_c.aram_slept += slept;
}
void port_lp_spin(u64 t) { s_c.spin += t; }
void port_lp_conv(u64 t) {
    s_c.conv += t;
    s_c.conv_n++;
}
void port_lp_retrace(u64 t) { s_c.retrace += t; }

static void report_window(u32 frame, const char* what) {
    const Hist* a = s_c.dvd;
    const Hist* b = s_start.dvd;
    u64 wall = plat_ticks() - s_t0;
    PLOG_I("[LOADPROF] %s %s: %lu frames, wall %lu ms | busy %lu ms retrace %lu ms | dvd %lu reads %lu KB wait %lu ms (sd %lu ms, baton wake %lu ms, "
           "%lu sequential) | aram %lu xfers %lu KB sim %lu ms slept %lu ms spin %lu ms | conv %lu files %lu ms",
           what, s_name, (unsigned long)(frame - s_f0), (unsigned long)(us(wall) / 1000u), (unsigned long)(us(s_busy_sum) / 1000u),
           (unsigned long)(us(s_c.retrace - s_start.retrace) / 1000u), (unsigned long)(a[HIST - 1].count - b[HIST - 1].count),
           (unsigned long)((a[HIST - 1].bytes - b[HIST - 1].bytes) / 1024u),
           (unsigned long)(us(a[HIST - 1].ticks - b[HIST - 1].ticks) / 1000u), (unsigned long)(us(s_c.sd - s_start.sd) / 1000u),
           (unsigned long)(us(s_c.wake - s_start.wake) / 1000u),
           (unsigned long)(a[HIST - 1].seq_count - b[HIST - 1].seq_count),
           (unsigned long)(s_c.aram_posts - s_start.aram_posts), (unsigned long)((s_c.aram_bytes - s_start.aram_bytes) / 1024u),
           (unsigned long)(us(s_c.aram_sim - s_start.aram_sim) / 1000u),
           (unsigned long)(us(s_c.aram_slept - s_start.aram_slept) / 1000u),
           (unsigned long)(us(s_c.spin - s_start.spin) / 1000u), (unsigned long)(s_c.conv_n - s_start.conv_n),
           (unsigned long)(us(s_c.conv - s_start.conv) / 1000u));
    char line[260];
    int n = 0;
    for (int i = 0; i < HIST - 1; i++) {
        u32 c = a[i].count - b[i].count;
        if (c == 0 || n >= (int)sizeof(line) - 60) {
            continue;
        }
        n += snprintf(line + n, sizeof(line) - n, " %s:%lu/%luKB/%luus(max %lu)/seq%lu", k_hist_name[i], (unsigned long)c,
                      (unsigned long)((a[i].bytes - b[i].bytes) / 1024u), (unsigned long)(us(a[i].ticks - b[i].ticks) / c),
                      (unsigned long)us(a[i].max_ticks), (unsigned long)(a[i].seq_count - b[i].seq_count));
    }
    PLOG_I("[LOADPROF] %s fs: %lu calls %lu KB (game reads %lu %lu KB; over-read %ld KB), read-ahead hits %lu, merged %lu", s_name,
           (unsigned long)(s_c.fs_calls - s_start.fs_calls), (unsigned long)((s_c.fs_bytes - s_start.fs_bytes) / 1024u),
           (unsigned long)(a[HIST - 1].count - b[HIST - 1].count), (unsigned long)((a[HIST - 1].bytes - b[HIST - 1].bytes) / 1024u),
           (long)(((long long)(s_c.fs_bytes - s_start.fs_bytes) - (long long)(a[HIST - 1].bytes - b[HIST - 1].bytes)) / 1024),
           (unsigned long)(s_c.ra_hits - s_start.ra_hits), (unsigned long)(s_c.merged - s_start.merged));
    PLOG_I("[LOADPROF] %s frames by reads finished in them: 0:%lu 1:%lu 2:%lu 3+:%lu", s_name,
           (unsigned long)(s_c.reads_per_frame[0] - s_start.reads_per_frame[0]),
           (unsigned long)(s_c.reads_per_frame[1] - s_start.reads_per_frame[1]),
           (unsigned long)(s_c.reads_per_frame[2] - s_start.reads_per_frame[2]),
           (unsigned long)(s_c.reads_per_frame[3] - s_start.reads_per_frame[3]));
    PLOG_I("[LOADPROF] %s dvd by size (count/bytes/mean latency):%s", s_name, line);
}

static void report_post(void) {
    char line[200];
    int n = 0;
    u64 sum = 0, steady = 0;
    u32 first = s_post_n < 6 ? s_post_n : 6;
    for (u32 i = 1; i <= first; i++) {
        n += snprintf(line + n, sizeof(line) - n, " %lu.%02lu", (unsigned long)(ms10(s_post_busy[i]) / 100), (unsigned long)(ms10(s_post_busy[i]) % 100));
    }
    u32 sn = s_post_n < POST_SUM ? s_post_n : POST_SUM;
    for (u32 i = 1; i <= sn; i++) {
        sum += s_post_busy[i];
    }
    u32 tn = 0;
    for (u32 i = 61; i <= s_post_n && i <= POST_FRAMES; i++, tn++) {
        steady += s_post_busy[i];
    }
    u64 mean_steady = tn != 0 ? steady / tn : 0;
    u64 excess = tn != 0 && sum > mean_steady * sn ? sum - mean_steady * sn : 0;
    PLOG_I("[LOADPROF] %s after (game-thread busy per frame, ms): first %lu:%s | sum of first %lu %lu ms | steady mean (frames 61-%d) %lu.%02lu ms "
           "| first-use excess %lu ms | dvd %lu reads %lu KB, conv %lu ms, aram %lu xfers in those frames",
           s_name, (unsigned long)first, line, (unsigned long)sn, (unsigned long)(us(sum) / 1000u), POST_FRAMES,
           (unsigned long)(ms10(mean_steady) / 100), (unsigned long)(ms10(mean_steady) % 100), (unsigned long)(us(excess) / 1000u),
           (unsigned long)(s_c.dvd[HIST - 1].count - s_post_start.dvd[HIST - 1].count),
           (unsigned long)((s_c.dvd[HIST - 1].bytes - s_post_start.dvd[HIST - 1].bytes) / 1024u),
           (unsigned long)(us(s_c.conv - s_post_start.conv) / 1000u), (unsigned long)(s_c.aram_posts - s_post_start.aram_posts));
}

void port_lp_frame(u32 frame, int seq, int ready, const char* map, u64 busy) {
    if (!port_lp_active()) {
        return;
    }
    s_c.sd = plat_disc_sd_ticks();
    plat_disc_stats(&s_c.fs_calls, &s_c.fs_bytes);
    s_c.ra_hits = plat_disc_cache_hits();
    s_c.merged = plat_disc_merged();
    {
        static u32 last_reads;
        u32 now_reads = s_c.dvd[HIST - 1].count;
        u32 d = now_reads - last_reads;
        last_reads = now_reads;
        s_c.reads_per_frame[d > 3 ? 3 : d]++;
    }
    if (s_state == 2) {
        s_post_n++;
        s_post_busy[s_post_n] = busy;
        if (s_post_n >= POST_FRAMES) {
            report_post();
            s_state = 0;
        }
    } else if (s_state == 1) {
        s_nframes++;
        s_busy_sum += busy;
    }
    if (s_state == 1 && ((s_kind == 0 && seq != SEQ_MAP_CHANGE) || (s_kind == 1 && ready) || frame - s_f0 > 36000)) {
        if (s_kind == 0 && map != NULL) {
            snprintf(s_name, sizeof(s_name), "%.15s", map); /* the map that came up */
        }
        report_window(frame, s_kind == 0 ? "mapchange" : "battle");
        s_state = 2;
        s_post_n = 0;
        s_post_start = s_c;
        s_post_end_frame = frame;
    }
    int start_kind = -1;
    if (seq == SEQ_MAP_CHANGE && s_prev_seq != SEQ_MAP_CHANGE) {
        start_kind = 0;
    } else if (seq == SEQ_BATTLE && s_prev_seq != SEQ_BATTLE) {
        start_kind = 1;
    }
    if (start_kind >= 0 && s_state != 1) {
        if (s_state == 2) {
            report_post();
        }
        s_state = 1;
        s_kind = start_kind;
        s_start = s_c;
        s_t0 = plat_ticks();
        s_f0 = frame;
        s_nframes = 0;
        s_busy_sum = 0;
        snprintf(s_name, sizeof(s_name), "%.15s", map != NULL ? map : "?");
        PLOG_I("[LOADPROF] %s %s requested at frame %lu", start_kind == 0 ? "mapchange" : "battle", s_name, (unsigned long)frame);
    }
    s_prev_seq = seq;
}
