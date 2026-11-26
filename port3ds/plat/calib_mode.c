/* In-game development tool: the CPU and memory calibration benchmark (switch "calib").
 *
 * Switch "calib" (development builds only; a release build ignores it) makes the process run the
 * benchmark INSTEAD of the game, like gpuprobe_mode.c: boot.c calls plat_calib_mode_run() after the log,
 * crash handler, watchdog and devupdate server are up and before any game or GPU init. It never returns:
 * after the run it keeps the devupdate server answering (restart, push, files get) until START or a relaunch.
 *
 *   calib               run the set
 *   calibms=N           target time of one timed run in ms (default 120; each benchmark is the best of 3)
 *   calibonly=text      only benchmarks whose name contains text (comma list)
 *   calibpmu            a second pass with the ARM11 counters (ctrdk_perf: cycles, data-cache read misses,
 *                       instruction-cache misses per op) when the way in works; written to calib_pmu.txt
 *
 * The purpose: the ratio between Azahar and the console (docs/PORT_3DS_60FPS.md, Step 2). The same
 * binary and the same set run on both; the ratio per row says what Azahar's times mean on the console.
 *
 * Output: sdmc:/3ds/ttyd/calib.txt; from the PC:   devupdate.py files get @/calib.txt
 * The log lines "CALIB <name> ..." give each row and "CALIB DONE ..." ends the run.
 *
 * Rows: iterations (operations), total ms of the best run, ns per operation, MB/s where the row moves
 * bytes. An "operation" is stated by the group: one loop iteration, one call, one copy of the size, one
 * 32-bit word read, one pointer hop. The SDK rows call the game's own functions (PSMTXConcat and so on,
 * the port's C_ versions) and ttyd_sincosf; the libm rows call newlib's, as the game does.
 */
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ctrdk/perf.h"
#include "plat_internal.h"

/* The game's own functions, linked from port3ds/game (the generated matrix library and src/system.c).
 * Plain floats and pointers cross the two halves. */
extern void PSMTXConcat(const float a[3][4], const float b[3][4], float ab[3][4]);
extern void PSMTXMultVec(const float m[3][4], const float* src, float* dst);
extern void PSVECNormalize(const float* src, float* dst);
extern unsigned PSMTXInverse(const float src[3][4], float inv[3][4]);
extern void PSMTXRotRad(float m[3][4], char axis, float rad);
extern void ttyd_sincosf(float* s, float* c, float degrees);

#define MAX_BENCH 96
#define QTM_VADDR 0x1E800000u
#define QTM_BYTES 0x00200000u

typedef struct Bench Bench;
struct Bench {
    const char* group;
    char name[40];
    uint32_t (*fn)(const Bench*, uint32_t n);
    uint8_t* a;
    uint8_t* b;
    uint32_t size;
    uint32_t opi;   /* operations per iteration of fn's loop */
    uint32_t bytes; /* bytes moved per operation (0: none) */
    /* results */
    int done;
    uint64_t iters; /* operations */
    uint64_t ticks; /* best run */
    uint32_t n_best;
    int pmu;
    uint64_t cyc, dmiss, imiss; /* per 1000 operations */
};

static Bench s_b[MAX_BENCH];
static int s_nb;
static int s_mode;
static volatile uint32_t s_sink;
static volatile float s_fsink;
static volatile float s_fin[4] = {0.9993f, 0.0007f, 1.00001f, 0.37f};
static int s_target_ms = 120;

int plat_calib_mode(void) { return s_mode; }
int plat_calib_requested(void) { return plat_is_dev_build() && plat_switch_has("calib"); }

static bool on_emulator(void) {
    s64 v = 0, lv = -1;
    bool citra = R_SUCCEEDED(svcGetSystemInfo(&v, 0x20000, 0)) && v == 1;
    bool lq = R_SUCCEEDED(svcGetSystemInfo(&lv, 0x10000, 0));
    return citra || (lq && lv == 0);
}

static void show(const char* l1, const char* l2) {
    consoleInit(GFX_TOP, NULL);
    printf("\n  TTYD 3DS port: calibration mode\n  build %s\n\n  %s\n  %s\n\n  The game does not start in this mode.\n"
           "  devupdate.py restart \"<switches>\" to leave.\n  START: exit\n",
           plat_build_string(), l1, l2);
    gfxFlushBuffers();
    gfxSwapBuffers();
}

/* ---- kernels: n iterations, a checksum into a sink so nothing is removed ---- */

static uint32_t xs(uint32_t* s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

static uint32_t k_int_alu(const Bench* b, uint32_t n) {
    uint32_t x = s_sink | 1;
    for (uint32_t i = 0; i < n; i++) {
        x = (x ^ (x >> 5)) + (x << 3) + i;
    }
    return x;
}
static uint32_t k_int_mul(const Bench* b, uint32_t n) {
    uint32_t x = s_sink | 1;
    for (uint32_t i = 0; i < n; i++) {
        x = x * 2654435761u + i;
    }
    return x;
}
static uint32_t k_int_div(const Bench* b, uint32_t n) {
    uint32_t x = s_sink | 0x80000001u, acc = 0;
    for (uint32_t i = 0; i < n; i++) {
        acc += (x + i) / ((i & 255) + 3);
        x = x * 69069u + 1;
    }
    return acc;
}
static uint32_t k_u64_div(const Bench* b, uint32_t n) {
    uint64_t x = 0x123456789ABCDEF1ull ^ s_sink;
    uint32_t acc = 0;
    for (uint32_t i = 0; i < n; i++) {
        acc += (uint32_t)(x / (uint32_t)((i & 1023) + 7));
        x = x * 6364136223846793005ull + 1442695040888963407ull;
    }
    return acc;
}

static uint32_t k_fdep(const Bench* b, uint32_t n) {
    float a = s_fin[2], m = s_fin[0], c = s_fin[1];
    for (uint32_t i = 0; i < n; i++) {
        a = a * m + c;
    }
    s_fsink = a;
    return (uint32_t)(a * 1000.0f);
}
static uint32_t k_findep(const Bench* b, uint32_t n) {
    float a0 = s_fin[2], a1 = a0 + 0.1f, a2 = a0 + 0.2f, a3 = a0 + 0.3f, m = s_fin[0], c = s_fin[1];
    for (uint32_t i = 0; i < n; i++) {
        a0 = a0 * m + c;
        a1 = a1 * m + c;
        a2 = a2 * m + c;
        a3 = a3 * m + c;
    }
    s_fsink = a0 + a1 + a2 + a3;
    return (uint32_t)(s_fsink * 1000.0f);
}
static uint32_t k_fdiv(const Bench* b, uint32_t n) {
    float a = 1000.0f, d = s_fin[2];
    for (uint32_t i = 0; i < n; i++) {
        a = a / d + 1.0f;
    }
    s_fsink = a;
    return (uint32_t)a;
}
static uint32_t k_sqrtf(const Bench* b, uint32_t n) {
    float a = 1000.0f + s_fin[3];
    for (uint32_t i = 0; i < n; i++) {
        a = sqrtf(a) + 30.0f;
    }
    s_fsink = a;
    return (uint32_t)a;
}
static uint32_t k_sinf(const Bench* b, uint32_t n) {
    float x = s_fin[3], acc = 0;
    for (uint32_t i = 0; i < n; i++) {
        acc += sinf(x);
        x += 0.013f;
        if (x > 6.2831853f) {
            x -= 6.2831853f;
        }
    }
    s_fsink = acc;
    return (uint32_t)(acc * 100.0f);
}
static uint32_t k_cosf(const Bench* b, uint32_t n) {
    float x = s_fin[3], acc = 0;
    for (uint32_t i = 0; i < n; i++) {
        acc += cosf(x);
        x += 0.013f;
        if (x > 6.2831853f) {
            x -= 6.2831853f;
        }
    }
    s_fsink = acc;
    return (uint32_t)(acc * 100.0f);
}
static uint32_t k_atan2f(const Bench* b, uint32_t n) {
    float x = s_fin[3], acc = 0;
    for (uint32_t i = 0; i < n; i++) {
        acc += atan2f(x, 1.5f);
        x += 0.013f;
        if (x > 50.0f) {
            x -= 100.0f;
        }
    }
    s_fsink = acc;
    return (uint32_t)(acc * 100.0f);
}
static uint32_t k_ttyd_sincosf(const Bench* b, uint32_t n) {
    float x = s_fin[3], s = 0, c = 0, acc = 0;
    for (uint32_t i = 0; i < n; i++) {
        ttyd_sincosf(&s, &c, x);
        acc += s + c;
        x += 0.37f;
        if (x > 360.0f) {
            x -= 360.0f;
        }
    }
    s_fsink = acc;
    return (uint32_t)(acc * 100.0f);
}

/* the 3x4 concat of C_MTXConcat, written out here */
static void mtx_concat_local(const float a[3][4], const float b[3][4], float ab[3][4]) {
    float t[3][4];
    for (int i = 0; i < 3; i++) {
        t[i][0] = a[i][0] * b[0][0] + a[i][1] * b[1][0] + a[i][2] * b[2][0];
        t[i][1] = a[i][0] * b[0][1] + a[i][1] * b[1][1] + a[i][2] * b[2][1];
        t[i][2] = a[i][0] * b[0][2] + a[i][1] * b[1][2] + a[i][2] * b[2][2];
        t[i][3] = a[i][0] * b[0][3] + a[i][1] * b[1][3] + a[i][2] * b[2][3] + a[i][3];
    }
    memcpy(ab, t, sizeof(t));
}
static void mtx_init(float m[3][4], float k) {
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 4; j++) {
            m[i][j] = (i == j) ? 1.0f : 0.0f;
        }
    }
    m[0][3] = k;
    m[1][3] = -k;
    m[2][3] = 0.5f * k;
    m[0][1] = 0.01f * k;
}
static uint32_t k_mtx_concat_local(const Bench* b, uint32_t n) {
    float a[3][4], c[3][4];
    mtx_init(a, 0.001f + s_fin[1]);
    mtx_init(c, 0.002f);
    for (uint32_t i = 0; i < n; i++) {
        mtx_concat_local((const float(*)[4])a, (const float(*)[4])c, a);
    }
    s_fsink = a[0][3];
    return (uint32_t)(a[1][3] * 1000.0f);
}
static uint32_t k_mtx_concat_sdk(const Bench* b, uint32_t n) {
    float a[3][4], c[3][4];
    mtx_init(a, 0.001f + s_fin[1]);
    mtx_init(c, 0.002f);
    for (uint32_t i = 0; i < n; i++) {
        PSMTXConcat((const float(*)[4])a, (const float(*)[4])c, a);
    }
    s_fsink = a[0][3];
    return (uint32_t)(a[1][3] * 1000.0f);
}
static uint32_t k_mtx_multvec_sdk(const Bench* b, uint32_t n) {
    float m[3][4], v[3] = {1.0f, 2.0f, 3.0f};
    mtx_init(m, 0.001f + s_fin[1]);
    for (uint32_t i = 0; i < n; i++) {
        PSMTXMultVec((const float(*)[4])m, v, v);
    }
    s_fsink = v[0];
    return (uint32_t)v[1];
}
static uint32_t k_mtx_inv_sdk(const Bench* b, uint32_t n) {
    float m[3][4], r[3][4];
    mtx_init(m, 0.5f + s_fin[1]);
    unsigned acc = 0;
    for (uint32_t i = 0; i < n; i++) {
        acc += PSMTXInverse((const float(*)[4])m, r);
        m[0][3] = r[0][3] + 0.5f;
    }
    s_fsink = r[0][3];
    return acc;
}
static uint32_t k_mtx_rot_sdk(const Bench* b, uint32_t n) {
    float m[3][4];
    float x = s_fin[3];
    for (uint32_t i = 0; i < n; i++) {
        PSMTXRotRad(m, 'y', x);
        x += 0.013f;
        if (x > 6.2831853f) {
            x -= 6.2831853f;
        }
    }
    s_fsink = m[0][0];
    return (uint32_t)(m[0][2] * 1000.0f);
}
static uint32_t k_vec_norm_local(const Bench* b, uint32_t n) {
    float v[3] = {1.0f, 2.0f, 3.0f + s_fin[1]};
    for (uint32_t i = 0; i < n; i++) {
        float m = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
        m = 1.0f / sqrtf(m);
        v[0] = v[0] * m + 0.5f;
        v[1] = v[1] * m + 0.25f;
        v[2] = v[2] * m + 0.125f;
    }
    s_fsink = v[0];
    return (uint32_t)(v[1] * 1000.0f);
}
static uint32_t k_vec_norm_sdk(const Bench* b, uint32_t n) {
    float v[3] = {1.0f, 2.0f, 3.0f + s_fin[1]};
    for (uint32_t i = 0; i < n; i++) {
        PSVECNormalize(v, v);
        v[0] += 0.5f;
        v[1] += 0.25f;
        v[2] += 0.125f;
    }
    s_fsink = v[0];
    return (uint32_t)(v[1] * 1000.0f);
}

/* memory */
static uint32_t k_memcpy(const Bench* b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        memcpy(b->b, b->a, b->size);
        s_sink = b->b[i & 63]; /* the copy is observable */
    }
    return b->b[b->size - 1];
}
static uint32_t k_memset(const Bench* b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        memset(b->a, (int)i, b->size);
        s_sink = b->a[i & 63];
    }
    return b->a[b->size - 1];
}
/* sequential 32-bit reads over `size` bytes; one iteration is a pass, one operation a word */
static uint32_t k_read_seq(const Bench* b, uint32_t n) {
    const uint32_t* p = (const uint32_t*)b->a;
    uint32_t words = b->size / 4, acc = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
        for (uint32_t j = 0; j < words; j += 8) {
            s0 += p[j] + p[j + 4];
            s1 += p[j + 1] + p[j + 5];
            s2 += p[j + 2] + p[j + 6];
            s3 += p[j + 3] + p[j + 7];
        }
        acc += s0 + s1 + s2 + s3;
    }
    return acc;
}
/* one 32-byte line per node, one hop per operation; the node's word 0 is the next node's byte offset */
static uint32_t k_chase(const Bench* b, uint32_t n) {
    const uint8_t* base = b->a;
    uint32_t off = 0;
    for (uint32_t i = 0; i < n; i++) {
        off = *(const uint32_t*)(base + off);
    }
    return off;
}
/* a branch on the loaded value as well: half of them go each way, unpredictably */
static uint32_t k_chase_branchy(const Bench* b, uint32_t n) {
    const uint8_t* base = b->a;
    uint32_t off = 0, acc = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = *(const uint32_t*)(base + off + 4);
        if (v & 0x100) {
            acc += v >> 9;
        } else {
            acc ^= v << 3;
        }
        off = *(const uint32_t*)(base + off);
    }
    return off + acc;
}
/* data-dependent branch over an array of bytes: random (unpredictable) or sorted (predictable) */
static uint32_t k_branch(const Bench* b, uint32_t n) {
    const uint8_t* p = b->a;
    uint32_t cnt = b->size, acc = 0;
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t j = 0; j < cnt; j++) {
            uint32_t v = p[j];
            if (v >= 128) {
                acc += v;
            }
        }
    }
    return acc;
}

/* ---- the table ---- */

static int wanted(const char* name) {
    char only[160];
    if (!plat_switch_value("calibonly", only, sizeof(only))) {
        return 1;
    }
    char* save = only;
    while (*save != '\0') {
        char* item = save;
        char* comma = strchr(item, ',');
        if (comma != NULL) {
            *comma = '\0';
            save = comma + 1;
        } else {
            save += strlen(item);
        }
        if (*item != '\0' && strstr(name, item) != NULL) {
            return 1;
        }
    }
    return 0;
}

static Bench* add(const char* group, const char* name, uint32_t (*fn)(const Bench*, uint32_t), uint8_t* a, uint8_t* b,
                  uint32_t size, uint32_t opi, uint32_t bytes) {
    if (s_nb >= MAX_BENCH || !wanted(name)) {
        return NULL;
    }
    Bench* r = &s_b[s_nb++];
    memset(r, 0, sizeof(*r));
    r->group = group;
    snprintf(r->name, sizeof(r->name), "%s", name);
    r->fn = fn;
    r->a = a;
    r->b = b;
    r->size = size;
    r->opi = opi ? opi : 1;
    r->bytes = bytes;
    return r;
}

static void fill_chase(uint8_t* base, uint32_t size) {
    uint32_t nodes = size / 32, seed = 0x2545F491u;
    uint32_t* perm = (uint32_t*)malloc(nodes * sizeof(uint32_t));
    if (perm == NULL) {
        return;
    }
    for (uint32_t i = 0; i < nodes; i++) {
        perm[i] = i;
    }
    for (uint32_t i = nodes - 1; i > 0; i--) { /* Sattolo: one cycle through all nodes */
        uint32_t j = xs(&seed) % i;
        uint32_t t = perm[i];
        perm[i] = perm[j];
        perm[j] = t;
    }
    for (uint32_t i = 0; i < nodes; i++) {
        uint32_t* node = (uint32_t*)(base + (size_t)perm[i] * 32);
        node[0] = perm[(i + 1) % nodes] * 32;
        node[1] = xs(&seed);
    }
    free(perm);
}

static int cmp_u8(const void* a, const void* b) { return (int)*(const uint8_t*)a - (int)*(const uint8_t*)b; }

static void size_label(char* out, size_t n, uint32_t size) {
    if (size >= (1u << 20)) {
        snprintf(out, n, "%luM", (unsigned long)(size >> 20));
    } else {
        snprintf(out, n, "%luK", (unsigned long)(size >> 10));
    }
}

static void build_table(void) {
    char nm[40], sz[8];
    add("cpu int", "int_alu", k_int_alu, NULL, NULL, 0, 1, 0);
    add("cpu int", "int_mul", k_int_mul, NULL, NULL, 0, 1, 0);
    add("cpu int", "int_div32", k_int_div, NULL, NULL, 0, 1, 0);
    add("cpu int", "int_div64", k_u64_div, NULL, NULL, 0, 1, 0);
    add("cpu float", "flt_madd_dep", k_fdep, NULL, NULL, 0, 1, 0);
    add("cpu float", "flt_madd_x4", k_findep, NULL, NULL, 0, 4, 0);
    add("cpu float", "flt_div", k_fdiv, NULL, NULL, 0, 1, 0);
    add("cpu float", "flt_sqrt", k_sqrtf, NULL, NULL, 0, 1, 0);
    add("libm", "sinf", k_sinf, NULL, NULL, 0, 1, 0);
    add("libm", "cosf", k_cosf, NULL, NULL, 0, 1, 0);
    add("libm", "atan2f", k_atan2f, NULL, NULL, 0, 1, 0);
    add("sdk", "game_sincosf", k_ttyd_sincosf, NULL, NULL, 0, 1, 0);
    add("sdk", "mtxconcat_local", k_mtx_concat_local, NULL, NULL, 0, 1, 0);
    add("sdk", "PSMTXConcat", k_mtx_concat_sdk, NULL, NULL, 0, 1, 0);
    add("sdk", "PSMTXMultVec", k_mtx_multvec_sdk, NULL, NULL, 0, 1, 0);
    add("sdk", "PSMTXInverse", k_mtx_inv_sdk, NULL, NULL, 0, 1, 0);
    add("sdk", "PSMTXRotRad", k_mtx_rot_sdk, NULL, NULL, 0, 1, 0);
    add("sdk", "vecnorm_local", k_vec_norm_local, NULL, NULL, 0, 1, 0);
    add("sdk", "PSVECNormalize", k_vec_norm_sdk, NULL, NULL, 0, 1, 0);

    /* memcpy and memset of 4 KB, 64 KB, 1 MB in FCRAM (heap and linear heap) and in QTM RAM */
    static const uint32_t sizes[3] = {4u << 10, 64u << 10, 1u << 20};
    for (int region = 0; region < 3; region++) {
        const char* rn = region == 0 ? "heap" : (region == 1 ? "linear" : "qtm");
        uint32_t qused = 0, qtotal = 0;
        if (region == 2) {
            if (!plat_qtm_available()) {
                continue;
            }
            plat_qtm_usage(&qused, &qtotal);
        }
        for (int si = 0; si < 3; si++) {
            uint32_t s = sizes[si];
            uint8_t* base = NULL;
            if (region == 0) {
                base = (uint8_t*)memalign(64, 2 * s);
            } else if (region == 1) {
                base = (uint8_t*)linearMemAlign(2 * s, 64);
            } else {
                uint32_t start = (qused + 63u) & ~63u; /* the log queue is placed in QTM first */
                if (start + 2 * s <= QTM_BYTES) {
                    base = (uint8_t*)QTM_VADDR + start;
                    qused = start + 2 * s; /* the next size starts after this one */
                }
            }
            if (base == NULL) {
                continue;
            }
            memset(base, 0x5A, 2 * s);
            size_label(sz, sizeof(sz), s);
            snprintf(nm, sizeof(nm), "memcpy_%s_%s", rn, sz);
            add("memory copy", nm, k_memcpy, base, base + s, s, 1, s);
            snprintf(nm, sizeof(nm), "memset_%s_%s", rn, sz);
            add("memory copy", nm, k_memset, base, NULL, s, 1, s);
        }
    }

    /* reads: L1-resident (16 KB), L2-resident (256 KB, 1 MB), streaming (16 MB); then the same buffers as pointer chains */
    static const uint32_t rsz[4] = {16u << 10, 256u << 10, 1u << 20, 16u << 20};
    uint8_t* rbuf[4];
    for (int i = 0; i < 4; i++) {
        rbuf[i] = (uint8_t*)memalign(64, rsz[i]);
        if (rbuf[i] == NULL) {
            continue;
        }
        memset(rbuf[i], 1, rsz[i]);
        size_label(sz, sizeof(sz), rsz[i]);
        snprintf(nm, sizeof(nm), "read_seq_%s", sz);
        add("memory read (op = 32-bit word)", nm, k_read_seq, rbuf[i], NULL, rsz[i], rsz[i] / 4, 4);
    }
    for (int i = 0; i < 4; i++) {
        if (rbuf[i] == NULL) {
            continue;
        }
        size_label(sz, sizeof(sz), rsz[i]);
        snprintf(nm, sizeof(nm), "chase_%s", sz);
        fill_chase(rbuf[i], rsz[i]);
        add("pointer chase (op = hop)", nm, k_chase, rbuf[i], NULL, rsz[i], 1, 0);
    }
    {
        uint8_t* base = (uint8_t*)memalign(64, 256u << 10);
        if (base != NULL) {
            fill_chase(base, 256u << 10);
            add("pointer chase (op = hop)", "chase_branchy_256K", k_chase_branchy, base, NULL, 256u << 10, 1, 0);
        }
    }
    {
        uint8_t* rnd = (uint8_t*)memalign(64, 16384);
        uint8_t* srt = (uint8_t*)memalign(64, 16384);
        if (rnd != NULL && srt != NULL) {
            uint32_t seed = 0x9E3779B9u;
            for (int i = 0; i < 16384; i++) {
                rnd[i] = (uint8_t)xs(&seed);
            }
            memcpy(srt, rnd, 16384);
            qsort(srt, 16384, 1, cmp_u8);
            add("branch (op = element)", "branch_random_16K", k_branch, rnd, NULL, 16384, 16384, 0);
            add("branch (op = element)", "branch_sorted_16K", k_branch, srt, NULL, 16384, 16384, 0);
        }
    }
}

/* ---- running ---- */

static uint64_t time_run(const Bench* b, uint32_t n) {
    uint64_t t0 = plat_ticks();
    uint32_t r = b->fn(b, n);
    uint64_t t1 = plat_ticks();
    s_sink = r;
    return t1 - t0;
}

static void run_one(Bench* b) {
    const uint64_t target = (uint64_t)s_target_ms * (PLAT_TICKS_PER_SEC / 1000u);
    uint32_t n = 1;
    uint64_t t = time_run(b, n);
    while (t < target / 8 && n < 0x10000000u) { /* grow until a run takes an eighth of the target */
        n *= 4;
        t = time_run(b, n);
    }
    if (t == 0) {
        t = 1;
    }
    uint64_t want = (uint64_t)n * target / t; /* scale to the target */
    n = want < 1 ? 1 : (want > 0x40000000u ? 0x40000000u : (uint32_t)want);
    uint64_t best = ~0ull;
    for (int k = 0; k < 3; k++) {
        t = time_run(b, n);
        if (t < best) {
            best = t;
        }
    }
    b->n_best = n;
    b->ticks = best;
    b->iters = (uint64_t)n * b->opi;
    b->done = 1;
}

/* "123.45" from a value, with no floating-point printf */
static void fmt2(char* out, size_t n, double v) {
    if (v < 0) {
        v = 0;
    }
    uint64_t x = (uint64_t)(v * 100.0 + 0.5);
    snprintf(out, n, "%lu.%02lu", (unsigned long)(x / 100), (unsigned long)(x % 100));
}

static double ns_per_op(const Bench* b) {
    return (double)b->ticks * 1e9 / (double)PLAT_TICKS_PER_SEC / (double)b->iters;
}

static char s_txt[24576];

static int emit_table(char* buf, size_t cap, int with_pmu) {
    int len = 0;
    const char* group = "";
    for (int i = 0; i < s_nb; i++) {
        const Bench* b = &s_b[i];
        if (!b->done || (with_pmu && !b->pmu)) {
            continue;
        }
        if (strcmp(group, b->group) != 0) {
            group = b->group;
            len += snprintf(buf + len, cap - len, "\n[%s]\n", group);
        }
        char a[24], c[24], d[24], e[24];
        double ms = (double)b->ticks * 1000.0 / (double)PLAT_TICKS_PER_SEC;
        fmt2(a, sizeof(a), ms);
        fmt2(c, sizeof(c), ns_per_op(b));
        if (with_pmu) {
            fmt2(d, sizeof(d), (double)b->cyc / 1000.0);
            fmt2(e, sizeof(e), (double)b->dmiss / 1000.0);
            char f[24];
            fmt2(f, sizeof(f), (double)b->imiss / 1000.0);
            len += snprintf(buf + len, cap - len, "%-22s cycles/op %10s  dmiss/op %9s  imiss/op %9s\n", b->name, d, e, f);
        } else if (b->bytes != 0) {
            double mbs = (double)b->iters * b->bytes / ((double)b->ticks / PLAT_TICKS_PER_SEC) / (1024.0 * 1024.0);
            fmt2(d, sizeof(d), mbs);
            len += snprintf(buf + len, cap - len, "%-22s %12lu ops %10s ms %12s ns/op %10s MB/s\n", b->name,
                            (unsigned long)b->iters, a, c, d);
        } else {
            len += snprintf(buf + len, cap - len, "%-22s %12lu ops %10s ms %12s ns/op\n", b->name, (unsigned long)b->iters, a, c);
        }
        if (len >= (int)cap - 200) {
            break;
        }
    }
    return len;
}

static void write_results(const char* file, const char* title, int with_pmu, int total_ms) {
    int len = snprintf(s_txt, sizeof(s_txt),
                       "TTYD 3DS calibration (%s)\nbuild %s\nrunning on: %s\ntarget %d ms a run, best of 3; %d benchmarks, %d ms in all\n",
                       title, plat_build_string(), on_emulator() ? "emulator" : "console", s_target_ms, s_nb, total_ms);
    len += emit_table(s_txt + len, sizeof(s_txt) - len, with_pmu);
    plat_sd_write_file(file, s_txt, (size_t)len, PLAT_SD_ATOMIC | PLAT_SD_FSYNC);
    plat_sd_flush();
}

static void pmu_pass(void) {
    ctrdk_perf_init(NULL);
    if (!ctrdk_perf_available()) {
        PLOG_I("CALIB pmu: not available (%s, error 0x%lx)", ctrdk_perf_backend_name(), (unsigned long)ctrdk_perf_last_error());
        return;
    }
    PLOG_I("CALIB pmu: on via %s", ctrdk_perf_backend_name());
    for (int i = 0; i < s_nb; i++) {
        Bench* b = &s_b[i];
        if (!b->done) {
            continue;
        }
        CtrdkPerfSample p0, p1, d;
        ctrdk_perf_read_core(&p0);
        uint32_t r = b->fn(b, b->n_best);
        ctrdk_perf_read_core(&p1);
        s_sink = r;
        ctrdk_perf_diff(&p0, &p1, &d);
        if (d.valid & 1) {
            b->pmu = 1;
            b->cyc = d.cycles * 1000 / b->iters;
            b->dmiss = d.pmn0 * 1000 / b->iters;
            b->imiss = d.pmn1 * 1000 / b->iters;
        }
    }
    ctrdk_perf_exit();
}

void plat_calib_mode_run(void) {
    s_mode = 1;
    PLOG_I("calib mode: the game does not start; switches \"%s\"", plat_switches_text());
    plat_set_phase("calib");
    gfxInitDefault();
    show("Running the benchmarks...", "(no input is needed)");
    s_target_ms = plat_switch_int("calibms", 120);
    if (s_target_ms < 10) {
        s_target_ms = 10;
    }
    plat_wd_hold(1); /* a long run makes no frames */
    uint64_t t0 = plat_ticks();
    build_table();
    PLOG_I("CALIB start: %d benchmarks, %d ms a run, on %s, QTM %s", s_nb, s_target_ms, on_emulator() ? "an emulator" : "the console",
           plat_qtm_available() ? "mapped" : "not mapped");
    for (int i = 0; i < s_nb; i++) {
        Bench* b = &s_b[i];
        run_one(b);
        char a[24], c[24];
        fmt2(a, sizeof(a), (double)b->ticks * 1000.0 / (double)PLAT_TICKS_PER_SEC);
        fmt2(c, sizeof(c), ns_per_op(b));
        PLOG_I("CALIB %-22s %12lu ops %9s ms %11s ns/op", b->name, (unsigned long)b->iters, a, c);
    }
    int total_ms = (int)((plat_ticks() - t0) / (PLAT_TICKS_PER_SEC / 1000u));
    write_results("calib.txt", "timing", 0, total_ms);
    int have_pmu = 0;
    if (plat_switch_has("calibpmu")) {
        pmu_pass();
        for (int i = 0; i < s_nb; i++) {
            have_pmu |= s_b[i].pmu;
        }
        if (have_pmu) {
            write_results("calib_pmu.txt", "ARM11 counters", 1, total_ms);
        }
    }
    plat_wd_hold(0);
    PLOG_I("CALIB DONE: %d benchmarks in %d ms%s; files get @/calib.txt", s_nb, total_ms, have_pmu ? ", calib_pmu.txt too" : "");

    char l1[64];
    snprintf(l1, sizeof l1, "CALIB DONE: %d benchmarks, %d ms", s_nb, total_ms);
    show(l1, "files get @/calib.txt");
    while (aptMainLoop()) {
        plat_wd_progress();
        plat_devupdate_frame(); /* pauses for an update and leaves for a relaunch */
        hidScanInput();
        if (hidKeysDown() & KEY_START) {
            break;
        }
        gspWaitForVBlank();
    }
    plat_quit(0);
}
