/* qtmbench: results (JSON and text tables), the log, and the screen.
 * SPDX-License-Identifier: MIT
 * Copyright (c) the ctr-devkit authors
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bench.h"
#include "ctrdk/devupdate.h"

typedef struct {
    char suite[16];
    char test[28];
    char mem[20];
    s8 core;
    u32 size;
    double value;
    char unit[8];
    double aux;
    char note[100];
} Rec;

#define MAX_RECS 6000
#define MAX_INFO 64

static Rec* s_recs;
static int s_nrec;
static char s_info_key[MAX_INFO][24];
static char s_info_val[MAX_INFO][120];
static int s_ninfo;
static FILE* s_log;
static LightLock s_lock;

void out_open(void) {
    LightLock_Init(&s_lock);
    s_recs = calloc(MAX_RECS, sizeof(Rec));
    remove(QB_DIR "/previous.log");
    rename(QB_DIR "/log.txt", QB_DIR "/previous.log");
    s_log = fopen(QB_DIR "/log.txt", "w");
}

void out_close(void) {
    if (s_log != NULL) {
        fclose(s_log);
        s_log = NULL;
    }
}

static void log_line(const char* line) {
    LightLock_Lock(&s_lock);
    if (s_log != NULL) {
        fputs(line, s_log);
        fputc('\n', s_log);
        fflush(s_log);
    }
    svcOutputDebugString(line, strlen(line));
    devupdate_queue_log_line(line);
    LightLock_Unlock(&s_lock);
}

void logf_(const char* fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    log_line(line);
}

/* Main thread only: the console is not thread-safe. */
void status(const char* fmt, ...) {
    static u64 t0;
    if (t0 == 0) {
        t0 = tick();
    }
    char line[256];
    int k = snprintf(line, sizeof(line), "[%5.1f s] ", t2us(tick() - t0) / 1e6);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + k, sizeof(line) - k, fmt, ap);
    va_end(ap);
    log_line(line);
    printf("%.49s\n", line);
    gfxFlushBuffers();
    gfxSwapBuffers();
}

void info(const char* key, const char* fmt, ...) {
    int i;
    for (i = 0; i < s_ninfo; i++) {
        if (strcmp(s_info_key[i], key) == 0) {
            break;
        }
    }
    if (i == MAX_INFO) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_info_val[i], sizeof(s_info_val[i]), fmt, ap);
    va_end(ap);
    snprintf(s_info_key[i], sizeof(s_info_key[i]), "%s", key);
    if (i == s_ninfo) {
        s_ninfo++;
    }
    logf_("[I] %s = %s", key, s_info_val[i]);
}

static void make_key(const Rec* r, char* buf, size_t n) {
    snprintf(buf, n, "%s/%s/%s/c%d/%lu", r->suite, r->test, r->mem, r->core, (unsigned long)r->size);
}

void rec(const char* suite, const char* test, const char* mem, int core, u32 size, double value, const char* unit,
         double aux, const char* note) {
    if (s_recs == NULL || s_nrec >= MAX_RECS) {
        return;
    }
    LightLock_Lock(&s_lock);
    Rec* r = &s_recs[s_nrec++];
    LightLock_Unlock(&s_lock);
    snprintf(r->suite, sizeof(r->suite), "%s", suite);
    snprintf(r->test, sizeof(r->test), "%s", test);
    snprintf(r->mem, sizeof(r->mem), "%s", mem != NULL ? mem : "-");
    r->core = (s8)core;
    r->size = size;
    r->value = value;
    snprintf(r->unit, sizeof(r->unit), "%s", unit);
    r->aux = aux;
    snprintf(r->note, sizeof(r->note), "%s", note != NULL ? note : "");
    char key[96];
    make_key(r, key, sizeof(key));
    if (isnan(aux)) {
        logf_("[R] %s %.4g %s %s", key, value, unit, r->note);
    } else {
        logf_("[R] %s %.4g %s (%.4g) %s", key, value, unit, aux, r->note);
    }
}

/* ---- JSON ---- */

static void json_str(FILE* f, const char* s) {
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') {
            fputc('\\', f);
            fputc(*s, f);
        } else if ((unsigned char)*s < 0x20) {
            fprintf(f, "\\u%04x", *s);
        } else {
            fputc(*s, f);
        }
    }
    fputc('"', f);
}

static void json_num(FILE* f, double v) {
    if (isnan(v) || isinf(v)) {
        fputs("null", f);
    } else {
        fprintf(f, "%.6g", v);
    }
}

static void write_json(const char* path) {
    FILE* f = fopen(path, "w");
    if (f == NULL) {
        logf_("results: cannot write %s", path);
        return;
    }
    fputs("{\n  \"schema\": 1,\n  \"tool\": \"qtmbench\",\n  \"info\": {", f);
    for (int i = 0; i < s_ninfo; i++) {
        fputs(i ? ",\n    " : "\n    ", f);
        json_str(f, s_info_key[i]);
        fputs(": ", f);
        json_str(f, s_info_val[i]);
    }
    fputs("\n  },\n  \"records\": [", f);
    for (int i = 0; i < s_nrec; i++) {
        const Rec* r = &s_recs[i];
        char key[96];
        make_key(r, key, sizeof(key));
        fputs(i ? ",\n    {" : "\n    {", f);
        fputs("\"key\": ", f);
        json_str(f, key);
        fputs(", \"suite\": ", f);
        json_str(f, r->suite);
        fputs(", \"test\": ", f);
        json_str(f, r->test);
        fputs(", \"mem\": ", f);
        json_str(f, r->mem);
        fprintf(f, ", \"core\": %d, \"size\": %lu, \"value\": ", r->core, (unsigned long)r->size);
        json_num(f, r->value);
        fputs(", \"unit\": ", f);
        json_str(f, r->unit);
        fputs(", \"aux\": ", f);
        json_num(f, r->aux);
        fputs(", \"note\": ", f);
        json_str(f, r->note);
        fputc('}', f);
    }
    fputs("\n  ]\n}\n", f);
    fclose(f);
}

/* ---- text tables ---- */

#define MAXCOL 12
#define MAXROW 40

static const char* fmt_size(u32 s, char* b, size_t n) {
    if (s == 0) {
        snprintf(b, n, "-");
    } else if (s >= QB_MIB && s % QB_MIB == 0) {
        snprintf(b, n, "%luM", (unsigned long)(s / QB_MIB));
    } else if (s >= 1024 && s % 1024 == 0) {
        snprintf(b, n, "%luK", (unsigned long)(s / 1024));
    } else {
        snprintf(b, n, "%lu", (unsigned long)s);
    }
    return b;
}

static int add_unique_str(char list[][20], int* n, int max, const char* s) {
    for (int i = 0; i < *n; i++) {
        if (strcmp(list[i], s) == 0) {
            return i;
        }
    }
    if (*n >= max) {
        return -1;
    }
    snprintf(list[*n], 20, "%s", s);
    return (*n)++;
}

/* by_size: one table per (test, core), rows = sizes; else one table per
 * core, rows = tests. Columns = mem. */
static void pivot(FILE* f, const char* suite, bool by_size) {
    char tests[MAXROW][20];
    int ntest = 0;
    int cores[8], ncore = 0;
    for (int i = 0; i < s_nrec; i++) {
        const Rec* r = &s_recs[i];
        if (strcmp(r->suite, suite) != 0) {
            continue;
        }
        add_unique_str(tests, &ntest, MAXROW, r->test);
        bool have = false;
        for (int c = 0; c < ncore; c++) {
            have |= cores[c] == r->core;
        }
        if (!have && ncore < 8) {
            cores[ncore++] = r->core;
        }
    }
    if (ntest == 0) {
        return;
    }
    fprintf(f, "\n== %s ==\n", suite);
    int ngroups = by_size ? ntest : 1;
    for (int g = 0; g < ngroups; g++) {
        for (int ci = 0; ci < ncore; ci++) {
            int core = cores[ci];
            char cols[MAXCOL][20];
            int ncol = 0;
            char rows[MAXROW][20];
            int nrow = 0;
            u32 sizes[MAXROW];
            int nsz = 0;
            const char* unit = "";
            for (int i = 0; i < s_nrec; i++) {
                const Rec* r = &s_recs[i];
                if (strcmp(r->suite, suite) != 0 || r->core != core || (by_size && strcmp(r->test, tests[g]) != 0)) {
                    continue;
                }
                add_unique_str(cols, &ncol, MAXCOL, r->mem);
                unit = r->unit;
                if (by_size) {
                    bool have = false;
                    for (int k = 0; k < nsz; k++) {
                        have |= sizes[k] == r->size;
                    }
                    if (!have && nsz < MAXROW) {
                        sizes[nsz++] = r->size;
                    }
                } else {
                    add_unique_str(rows, &nrow, MAXROW, r->test);
                }
            }
            if (ncol == 0) {
                continue;
            }
            if (by_size) {
                for (int a = 1; a < nsz; a++) { /* sort sizes */
                    for (int b = a; b > 0 && sizes[b - 1] > sizes[b]; b--) {
                        u32 t = sizes[b];
                        sizes[b] = sizes[b - 1];
                        sizes[b - 1] = t;
                    }
                }
                nrow = nsz;
            }
            char head[128];
            if (by_size) {
                snprintf(head, sizeof(head), "%.19s core %d [%.7s]", tests[g], core, unit);
            } else {
                snprintf(head, sizeof(head), "core %d", core);
            }
            fprintf(f, "\n%-24s", head);
            for (int c = 0; c < ncol; c++) {
                fprintf(f, " %11s", cols[c]);
            }
            fputc('\n', f);
            for (int ro = 0; ro < nrow; ro++) {
                char lab[24];
                if (by_size) {
                    fmt_size(sizes[ro], lab, sizeof(lab));
                } else {
                    snprintf(lab, sizeof(lab), "%s", rows[ro]);
                }
                fprintf(f, "%-24s", lab);
                for (int c = 0; c < ncol; c++) {
                    const Rec* hit = NULL;
                    for (int i = 0; i < s_nrec; i++) {
                        const Rec* r = &s_recs[i];
                        if (strcmp(r->suite, suite) == 0 && r->core == core && strcmp(r->mem, cols[c]) == 0 &&
                            (by_size ? (strcmp(r->test, tests[g]) == 0 && r->size == sizes[ro])
                                     : strcmp(r->test, rows[ro]) == 0)) {
                            hit = r;
                        }
                    }
                    if (hit == NULL) {
                        fprintf(f, " %11s", "");
                    } else if (by_size) {
                        fprintf(f, " %11.1f", hit->value);
                    } else {
                        char v[24];
                        snprintf(v, sizeof(v), "%.4g%s", hit->value, hit->unit);
                        fprintf(f, " %11s", v);
                    }
                }
                fputc('\n', f);
            }
        }
    }
}

static void flat(FILE* f, const char* suite) {
    bool any = false;
    for (int i = 0; i < s_nrec; i++) {
        const Rec* r = &s_recs[i];
        if (strcmp(r->suite, suite) != 0) {
            continue;
        }
        if (!any) {
            fprintf(f, "\n== %s ==\n", suite);
            any = true;
        }
        char sz[16];
        fmt_size(r->size, sz, sizeof(sz));
        char core[8] = "-";
        if (r->core >= 0) {
            snprintf(core, sizeof(core), "c%d", r->core);
        }
        fprintf(f, "%-22s %-18s %-3s %5s %12.4g %-6s", r->test, r->mem, core, sz, r->value, r->unit);
        if (!isnan(r->aux)) {
            fprintf(f, " (%.4g)", r->aux);
        }
        fprintf(f, " %s\n", r->note);
    }
}

static void write_txt(const char* path) {
    FILE* f = fopen(path, "w");
    if (f == NULL) {
        return;
    }
    fputs("qtmbench results (sdmc:/3ds/qtmbench/results.json has every number)\n", f);
    for (int i = 0; i < s_ninfo; i++) {
        fprintf(f, "%-20s %s\n", s_info_key[i], s_info_val[i]);
    }
    fputs("\nBandwidth in MB/s (1 MB = 10^6 bytes), latency in ns per load; rows are working sets.\n", f);
    pivot(f, "cpu_bw", true);
    pivot(f, "cpu_lat", true);
    pivot(f, "cpu_rand", true);
    pivot(f, "cache", true);
    flat(f, "cpu_xcopy");
    flat(f, "conc");
    pivot(f, "gpu_read", false);
    pivot(f, "gpu_write", false);
    flat(f, "gpu_check");
    pivot(f, "coherency", false);
    fputs("\nContention: slowdown factor of the victim (row) under the aggressor (column);\n"
          "1.00 = unaffected, 2.00 = takes twice as long. Raw numbers in the contend_raw list.\n",
          f);
    pivot(f, "contend", false);
    flat(f, "contend_raw");
    flat(f, "perf");
    flat(f, "map");
    flat(f, "sys");
    fclose(f);
}

void out_write(void) {
    write_json(QB_DIR "/results.json");
    write_txt(QB_DIR "/results.txt");
    logf_("results: %d records -> " QB_DIR "/results.json, results.txt", s_nrec);
}

bool suite_on(const char* name) {
    if (g_opt.only[0] == '\0') {
        return true;
    }
    size_t n = strlen(name);
    for (const char* p = g_opt.only; (p = strstr(p, name)) != NULL; p += n) {
        if ((p == g_opt.only || p[-1] == ',') && (p[n] == '\0' || p[n] == ',')) {
            return true;
        }
    }
    return false;
}
