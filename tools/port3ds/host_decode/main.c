/* host_decode: runs a game's disc-format converters on the host, on the files of a disc image.
 *
 *   host_decode [--disc IMAGE] list [-l]
 *   host_decode [--disc IMAGE] decode PATH [-o OUT] [--format NAME]
 *   host_decode [--disc IMAGE] all [--out DIR] [--tsv FILE] [--only NAME] [-q]
 *   host_decode [--disc IMAGE] discdata
 *   host_decode selftest
 *
 * See docs/PORT_3DS_HOST_DECODE.md. The converters are the port's own sources; main.c, fst.c and shim.c are
 * game-independent, and the converter table comes from the game (formats_<game>.c). */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "host_decode.h"
#include "port3ds/port_swap.h"
#include "port_swap_gen.h"
#include "port_disc_data_gen.h"
#include "port_formats.h"

#define MAX_FORMATS 32

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static void die(const char* fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
static void die(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("host_decode: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(2);
}

static void mkdirs(const char* path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
}

static int write_file(const char* path, const void* data, size_t n) {
    mkdirs(path);
    FILE* f = fopen(path, "wb");
    if (f == NULL) {
        return -1;
    }
    size_t w = fwrite(data, 1, n, f);
    fclose(f);
    return w == n ? 0 : -1;
}

/* ---- decoding one file ---- */

typedef struct {
    const HostFormat* fmt;
    const char* path;
    uint8_t* data; /* the buffer the converter works on */
    uint32_t size; /* bytes given to it */
    /* results */
    int failed;
    int unchanged; /* the converter left every byte as it was */
    char msg[512];
    double ms;
} Job;

static void convert_fn(void* arg) {
    Job* j = arg;
    j->fmt->convert(j->data, j->fmt->size_arg ? j->fmt->size_arg : j->size, j->path);
}

/* Reads the file as the game does (rounded up, zero padded, 32-byte aligned) and converts it. The buffer stays
 * in job->data for the caller to free. */
static int run_job(Job* j, uint32_t entry, double* read_ms) {
    const FstEntry* e = host_fst_entry(entry);
    uint32_t n = e->size;
    if (j->fmt->read_max != 0 && n > j->fmt->read_max) {
        n = j->fmt->read_max;
    }
    uint32_t size = n;
    if (j->fmt->read_pad != 0) {
        size = (n + j->fmt->read_pad - 1) / j->fmt->read_pad * j->fmt->read_pad;
    }
    uint8_t* buf = aligned_alloc(32, ((size_t)size + 64 + 31) / 32 * 32);
    if (buf == NULL) {
        die("out of memory for %s (%u bytes)", j->path, size);
    }
    memset(buf, 0, (size_t)size + 64);
    double t0 = now_ms();
    int32_t got = host_disc_read(e->offset, buf, n);
    *read_ms += now_ms() - t0;
    j->data = buf;
    j->size = size;
    if (got != (int32_t)n) {
        j->failed = 1;
        snprintf(j->msg, sizeof(j->msg), "short read (%d of %u bytes)", (int)got, n);
        return 1;
    }
    uint8_t* before = malloc(size);
    memcpy(before, buf, size);
    t0 = now_ms();
    j->failed = host_guard_run(convert_fn, j, j->msg, sizeof(j->msg));
    j->ms = now_ms() - t0;
    j->unchanged = memcmp(before, buf, size) == 0;
    free(before);
    if (!j->failed && j->fmt->idempotent) {
        /* converting converted data again must do nothing */
        uint8_t* copy = malloc(size);
        memcpy(copy, buf, size);
        char msg[512];
        Job again = *j;
        again.data = copy;
        int bad = host_guard_run(convert_fn, &again, msg, sizeof(msg));
        if (bad) {
            j->failed = 1;
            snprintf(j->msg, sizeof(j->msg), "second conversion stopped: %s", msg);
        } else if (memcmp(copy, buf, size) != 0) {
            size_t k = 0, diff = 0;
            for (; k < size; k++) {
                diff += copy[k] != buf[k];
            }
            j->failed = 1;
            snprintf(j->msg, sizeof(j->msg), "not idempotent: a second conversion changed %zu bytes", diff);
        }
        free(copy);
    }
    return j->failed;
}

static void path_of(uint32_t i, char* out, size_t n) { host_fst_path(i, out, n); }

static const HostFormat* find_format(const char* name) {
    for (int i = 0; i < host_format_count; i++) {
        if (strcmp(host_formats[i].name, name) == 0) {
            return &host_formats[i];
        }
    }
    return NULL;
}

/* ---- commands ---- */

static void cmd_list(int verbose) {
    char path[512];
    for (uint32_t i = 1; i < host_fst_count(); i++) {
        const FstEntry* e = host_fst_entry(i);
        path_of(i, path, sizeof(path));
        if (e->is_dir) {
            if (verbose) {
                printf("%10s %10s  %s/\n", "-", "-", path);
            }
            continue;
        }
        if (verbose) {
            char tags[256] = "";
            for (int f = 0; f < host_format_count; f++) {
                if (host_formats[f].match(path)) {
                    size_t n = strlen(tags);
                    snprintf(tags + n, sizeof(tags) - n, "%s%s", n ? "," : "", host_formats[f].name);
                }
            }
            printf("%08x %10u  %s%s%s%s\n", e->offset, e->size, path, tags[0] ? "  [" : "", tags, tags[0] ? "]" : "");
        } else {
            printf("%u %s\n", e->size, path);
        }
    }
}

static int cmd_decode(const char* path, const char* out, const char* fmt_name) {
    int32_t entry = host_fst_find(path);
    if (entry <= 0 || host_fst_entry((uint32_t)entry)->is_dir) {
        fprintf(stderr, "host_decode: %s is not a file on the disc\n", path);
        return 2;
    }
    char canon[512];
    path_of((uint32_t)entry, canon, sizeof(canon));
    const HostFormat* fmt = NULL;
    if (fmt_name != NULL) {
        fmt = find_format(fmt_name);
        if (fmt == NULL) {
            die("no format %s", fmt_name);
        }
    } else {
        for (int i = 0; i < host_format_count && fmt == NULL; i++) {
            if (host_formats[i].match(canon)) {
                fmt = &host_formats[i];
            }
        }
    }
    if (fmt == NULL) {
        fprintf(stderr, "host_decode: %s has no converter (it is used as it is)\n", canon);
        return 3;
    }
    Job j = {.fmt = fmt, .path = canon};
    double read_ms = 0;
    run_job(&j, (uint32_t)entry, &read_ms);
    if (j.failed) {
        fprintf(stderr, "FAIL %s %s: %s\n", fmt->name, canon, j.msg);
        free(j.data);
        return 1;
    }
    char dst[1024];
    if (out != NULL) {
        snprintf(dst, sizeof(dst), "%s", out);
    } else {
        snprintf(dst, sizeof(dst), "decoded/%s", canon);
    }
    if (write_file(dst, j.data, j.size) != 0) {
        die("cannot write %s: %s", dst, strerror(errno));
    }
    printf("%s %s -> %s (%u bytes, %.2f ms)\n", fmt->name, canon, dst, j.size, j.ms);
    free(j.data);
    return 0;
}

typedef struct {
    uint32_t files, failed, unchanged;
    double ms, read_ms;
    uint64_t bytes;
} Stat;

static int cmd_all(const char* outdir, const char* tsv, const char* only, int quiet) {
    Stat st[MAX_FORMATS] = {{0}};
    if (host_format_count > MAX_FORMATS) {
        die("too many formats");
    }
    FILE* tf = tsv ? fopen(tsv, "w") : NULL;
    if (tsv && tf == NULL) {
        die("cannot write %s", tsv);
    }
    uint32_t unconverted = 0;
    uint64_t unconverted_bytes = 0;
    char path[512];
    uint32_t nfiles = 0, nfail = 0;
    double t_all = now_ms();
    for (uint32_t i = 1; i < host_fst_count(); i++) {
        const FstEntry* e = host_fst_entry(i);
        if (e->is_dir) {
            continue;
        }
        nfiles++;
        path_of(i, path, sizeof(path));
        int matched = 0;
        for (int f = 0; f < host_format_count; f++) {
            const HostFormat* fmt = &host_formats[f];
            if (!fmt->match(path) || (only != NULL && strcmp(only, fmt->name) != 0)) {
                continue;
            }
            matched = 1;
            Job j = {.fmt = fmt, .path = path};
            run_job(&j, i, &st[f].read_ms);
            st[f].files++;
            st[f].bytes += e->size;
            st[f].ms += j.ms;
            st[f].unchanged += !j.failed && j.unchanged;
            if (j.failed) {
                st[f].failed++;
                nfail++;
                if (!quiet) {
                    printf("FAIL %-12s %s: %s\n", fmt->name, path, j.msg);
                }
            } else if (outdir != NULL) {
                char dst[1024];
                snprintf(dst, sizeof(dst), "%s/%s/%s", outdir, fmt->name, path);
                if (write_file(dst, j.data, j.size) != 0) {
                    die("cannot write %s", dst);
                }
            }
            if (tf) {
                for (char* c = j.msg; *c; c++) {
                    if (*c == '\t' || *c == '\n') {
                        *c = ' ';
                    }
                }
                fprintf(tf, "%s\t%s\t%s\t%u\t%.3f\t%s\n", fmt->name, path, j.failed ? "FAIL" : "ok", e->size, j.ms, j.msg);
            }
            free(j.data);
        }
        if (!matched) {
            unconverted++;
            unconverted_bytes += e->size;
            if (tf) {
                fprintf(tf, "-\t%s\tskip\t%u\t0\t\n", path, e->size);
            }
        }
    }
    double total = now_ms() - t_all;
    printf("\n%-14s %7s %7s %9s %12s %10s %10s\n", "format", "files", "failed", "unchanged", "bytes", "convert ms",
           "read ms");
    double ms_sum = 0;
    for (int f = 0; f < host_format_count; f++) {
        if (st[f].files == 0) {
            continue;
        }
        printf("%-14s %7u %7u %9u %12llu %10.1f %10.1f\n", host_formats[f].name, st[f].files, st[f].failed,
               st[f].unchanged,
               (unsigned long long)st[f].bytes, st[f].ms, st[f].read_ms);
        ms_sum += st[f].ms;
    }
    printf("\n%u files on the disc, %u with no converter (%llu bytes, used as they are), %u failures\n", nfiles,
           unconverted, (unsigned long long)unconverted_bytes, nfail);
    printf("converter time %.1f ms, wall time %.2f s\n", ms_sum, total / 1000.0);
    if (tf) {
        fclose(tf);
    }
    return nfail ? 1 : 0;
}

/* ---- discdata: the arrays the port fills from the image ---- */

void port_disc_data_register(void* dst, const char* rel, int section, unsigned int offset, unsigned int size);
void port_disc_data_fill(void);

static uint32_t fnv1a(const uint8_t* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

typedef struct {
    const char* key;
    void* buf;
    unsigned size;
} DiscArray;

static DiscArray s_arrays[16];
static int s_narrays;

static void add_array(const char* key, const char* rel, int section, unsigned offset, unsigned size) {
    void* buf = calloc(1, size);
    s_arrays[s_narrays++] = (DiscArray){key, buf, size};
    port_disc_data_register(buf, rel, section, offset, size);
}

extern uint8_t _mariost_icon_tex[], _mariost_icon_tlut[], _mariost_banner_tex[];

static int fill_fn_done;
static void fill_fn(void* arg) {
    (void)arg;
    port_disc_data_fill();
    fill_fn_done = 1;
}

#define ADD(key) add_array(#key, PORT_DISC_##key)

static int cmd_discdata(void) {
    /* disc_data.c registers the memory card art itself (its constructors); the game's own arrays are stood in
     * for by buffers. */
    s_arrays[s_narrays++] = (DiscArray){"mariost_icon_tex", _mariost_icon_tex, 0x400};
    s_arrays[s_narrays++] = (DiscArray){"mariost_icon_tlut", _mariost_icon_tlut, 0x200};
    s_arrays[s_narrays++] = (DiscArray){"mariost_banner_tex", _mariost_banner_tex, 0x1800};
    ADD(mapdrv_texture_0);
    ADD(torch_obj2_texture);
    ADD(torch_obj22t_texture);
    ADD(cubeDN);
    ADD(win_texture_0);
    char msg[256];
    if (host_guard_run(fill_fn, NULL, msg, sizeof(msg)) || !fill_fn_done) {
        printf("FAIL discdata: %s\n", msg);
        return 1;
    }
    for (int i = 0; i < s_narrays; i++) {
        printf("%s %u %08x\n", s_arrays[i].key, s_arrays[i].size, fnv1a(s_arrays[i].buf, s_arrays[i].size));
    }
    return 0;
}

/* ---- selftest: swap tables applied twice are the identity ---- */

static uint64_t s_rng = 0x9E3779B97F4A7C15ull;
static uint8_t rnd(void) {
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 7;
    s_rng ^= s_rng << 17;
    return (uint8_t)(s_rng >> 24);
}

static int cmd_selftest(void) {
    int bad = 0;
    for (unsigned t = 0; t < port_swap_all_count; t++) {
        const PortSwapType* ty = port_swap_all_types[t];
        for (int round = 0; round < 8; round++) {
            enum { N = 3 };
            uint32_t size = ty->size * N;
            uint8_t* a = malloc(size), *b = malloc(size);
            for (uint32_t i = 0; i < size; i++) {
                a[i] = b[i] = rnd();
            }
            /* two passes, each with its own PortSwap, as two files converted in a row */
            for (int pass = 0; pass < 2; pass++) {
                PortSwap s;
                port_swap_begin(&s, b, size, ty->name);
                port_swap_array(&s, 0, N, ty);
                port_swap_end(&s);
                if (pass == 0) {
                    /* the first pass must have changed exactly the bytes of the runs: unit swaps are involutions
                     * and bytes outside every run are untouched */
                    uint8_t* expect = malloc(size);
                    memcpy(expect, a, size);
                    for (uint32_t k = 0; k < N; k++) {
                        for (uint32_t r = 0; r < ty->nruns; r++) {
                            const PortSwapRun* run = &ty->runs[r];
                            for (uint32_t c = 0; c < run->count; c++) {
                                uint8_t* u = expect + k * ty->size + run->offset + c * run->width;
                                for (uint32_t x = 0; x < run->width / 2; x++) {
                                    uint8_t tmp = u[x];
                                    u[x] = u[run->width - 1 - x];
                                    u[run->width - 1 - x] = tmp;
                                }
                            }
                        }
                    }
                    if (memcmp(expect, b, size) != 0) {
                        printf("FAIL selftest %s: one pass is not a byte reversal of each run\n", ty->name);
                        bad++;
                    }
                    free(expect);
                }
            }
            if (memcmp(a, b, size) != 0) {
                printf("FAIL selftest %s: swapping twice is not the identity\n", ty->name);
                bad++;
            }
            free(a);
            free(b);
        }
    }
    /* units and runs */
    for (unsigned width = 2; width <= 8; width *= 2) {
        uint8_t a[64], b[64];
        for (int i = 0; i < 64; i++) {
            a[i] = b[i] = rnd();
        }
        for (int pass = 0; pass < 2; pass++) {
            PortSwap s;
            port_swap_begin(&s, b, 64, "units");
            port_swap_units(&s, 0, 64 / width, width);
            port_swap_end(&s);
        }
        if (memcmp(a, b, 64) != 0) {
            printf("FAIL selftest units(%u)\n", width);
            bad++;
        }
    }
    {
        uint8_t a[64], b[64];
        for (int i = 0; i < 64; i++) {
            a[i] = b[i] = rnd();
        }
        portSwapU16Run(b, 32);
        portSwapU16Run(b, 32);
        portSwapU32Run(b, 16);
        portSwapU32Run(b, 16);
        if (memcmp(a, b, 64) != 0) {
            printf("FAIL selftest portSwapU16Run/U32Run\n");
            bad++;
        }
    }
    printf("selftest: %u struct tables, %d failures\n", port_swap_all_count, bad);
    return bad ? 1 : 0;
}

/* ---- main ---- */

int main(int argc, char** argv) {
    const char* disc = getenv("TTYD_DISC");
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "--disc") && i + 1 < argc) {
            disc = argv[++i];
        } else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "-vv")) {
            host_set_log_level(argv[i][2] ? 3 : 1); /* -v: warnings, -vv: everything */
        } else {
            die("unknown option %s", argv[i]);
        }
    }
    if (i >= argc) {
        fputs("usage: host_decode [--disc IMAGE] [-v|-vv] list [-l] | decode PATH [-o OUT] [--format NAME] |\n"
              "       all [--out DIR] [--tsv FILE] [--only NAME] [-q] | discdata | selftest\n"
              "       (the image defaults to $TTYD_DISC)\n",
              stderr);
        return 2;
    }
    const char* cmd = argv[i++];
    if (!strcmp(cmd, "selftest")) {
        return cmd_selftest();
    }
    if (disc == NULL) {
        die("no disc image: pass --disc IMAGE or set TTYD_DISC");
    }
    char err[256];
    if (host_disc_open(disc, err, sizeof(err)) != 0) {
        die("%s", err);
    }
    const char *out = NULL, *tsv = NULL, *only = NULL, *fmt = NULL;
    int quiet = 0, verbose = 0;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) {
            out = argv[++i];
        } else if (!strcmp(argv[i], "--out") && i + 1 < argc) {
            out = argv[++i];
        } else if (!strcmp(argv[i], "--tsv") && i + 1 < argc) {
            tsv = argv[++i];
        } else if (!strcmp(argv[i], "--only") && i + 1 < argc) {
            only = argv[++i];
        } else if (!strcmp(argv[i], "--format") && i + 1 < argc) {
            fmt = argv[++i];
        } else if (!strcmp(argv[i], "-q")) {
            quiet = 1;
        } else if (!strcmp(argv[i], "-l")) {
            verbose = 1;
        } else if (argv[i][0] != '-') {
            break; /* positional: the decode path */
        } else {
            die("unknown option %s", argv[i]);
        }
    }
    if (!strcmp(cmd, "list")) {
        cmd_list(verbose);
        return 0;
    }
    if (!strcmp(cmd, "all")) {
        return cmd_all(out, tsv, only, quiet);
    }
    if (!strcmp(cmd, "discdata")) {
        return cmd_discdata();
    }
    if (!strcmp(cmd, "decode")) {
        if (i >= argc) {
            die("decode needs a path on the disc");
        }
        const char* path = argv[i++];
        for (; i < argc; i++) {
            if (!strcmp(argv[i], "-o") && i + 1 < argc) {
                out = argv[++i];
            } else if (!strcmp(argv[i], "--format") && i + 1 < argc) {
                fmt = argv[++i];
            } else {
                die("unknown option %s", argv[i]);
            }
        }
        return cmd_decode(path, out, fmt);
    }
    die("unknown command %s", cmd);
}
