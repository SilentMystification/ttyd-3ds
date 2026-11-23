/* ctr-devkit devupdate: the test-run queue. See include/ctrdk/devupdate.h.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) the ctr-devkit authors
 *
 * Files in files_dir:
 *   queue.txt    one entry per line, seven tab-separated fields:
 *                name, switches, padfile, until, frames, timeout_s, collect
 *                (padfile: a file name in files_dir; until: a POSIX extended
 *                regular expression; frames: end after that many app frames;
 *                timeout_s: seconds from process start; collect: comma-separated
 *                paths moved to out/<run>_<N>_<name>/ when the entry ends)
 *   state.txt    state=idle|running|done, index=N, total=T, run=K
 *   results.txt  one line per finished entry, tab-separated: N, name, result,
 *                frames, ms, outdir, detail
 *   out/         what the entries collected, and crash.txt for a crash
 * The state of a running queue travels in the launch arguments (ctrdkq=N), not
 * in files, so a cold start from the HOME Menu never continues a queue.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "internal.h"

#define LINE_MAX_LEN 1000
#define NAME_MAX_LEN 40
#define FIELDS 7
#define DEFAULT_TIMEOUT_S 300

typedef struct {
    char name[NAME_MAX_LEN + 1];
    char* sw;
    char* pad;
    char* until;
    char* collect;
    uint32_t frames;
    uint32_t timeout_s;
} Entry;

static char s_line[LINE_MAX_LEN + 1]; /* the running entry's fields point in here */
static Entry s_e;
static int s_n;     /* the running entry, 1-based; 0 = this launch is not a queue run */
static int s_total;
static int s_run;
static bool s_re_ok;
static uint64_t s_t0;
static uint32_t s_frames;
static volatile int s_fin; /* 0 running, 1 end pending, 2 recorded */
static char s_result[24];

/* ---- A small regular-expression matcher (newlib for the 3DS has no regcomp) ----
 * Search semantics. Supported: literal characters, . (any), [set] and [^set]
 * with ranges, \d \w \s and \x for a literal x, the postfix operators * + ? on
 * one of those, ^ and $ at the ends, and | between alternatives at the top
 * level. Not supported (refused when queued): groups ( ) and counts { }. */

static const char* re_atom_next(const char* p) {
    if (*p == '\\' && p[1] != '\0') {
        return p + 2;
    }
    if (*p == '[') {
        const char* q = p + 1;
        if (*q == '^') {
            q++;
        }
        if (*q == ']') {
            q++;
        }
        while (*q != '\0' && *q != ']') {
            q += (*q == '\\' && q[1] != '\0') ? 2 : 1;
        }
        return *q == ']' ? q + 1 : q;
    }
    return p + 1;
}

static bool re_esc_match(char e, char c) {
    switch (e) {
    case 'd': return c >= '0' && c <= '9';
    case 'w': return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    case 's': return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    default: return c == e;
    }
}

static bool re_atom_match(const char* p, char c) {
    if (*p == '.') {
        return true;
    }
    if (*p == '\\') {
        return re_esc_match(p[1], c);
    }
    if (*p != '[') {
        return *p == c;
    }
    const char* end = re_atom_next(p) - 1; /* the ']' */
    const char* q = p + 1;
    bool neg = *q == '^';
    if (neg) {
        q++;
    }
    bool hit = false;
    while (q < end) {
        char lo = *q;
        if (lo == '\\' && q + 1 < end) {
            hit |= re_esc_match(q[1], c);
            q += 2;
        } else if (q + 2 < end && q[1] == '-') {
            hit |= c >= lo && c <= q[2];
            q += 3;
        } else {
            hit |= c == lo;
            q++;
        }
    }
    return hit != neg;
}

static bool re_branch_end(const char* p) { return *p == '\0' || *p == '|'; }

static bool re_here(const char* p, const char* t) {
    if (re_branch_end(p)) {
        return true;
    }
    if (*p == '$' && re_branch_end(p + 1)) {
        return *t == '\0' || (*t == '\n' && t[1] == '\0');
    }
    const char* next = re_atom_next(p);
    char op = *next;
    if (op == '*' || op == '+' || op == '?') {
        int min = op == '+' ? 1 : 0, max = op == '?' ? 1 : 1 << 20, n = 0;
        while (n < max && t[n] != '\0' && re_atom_match(p, t[n])) {
            n++;
        }
        for (; n >= min; n--) {
            if (re_here(next + 1, t + n)) {
                return true;
            }
        }
        return false;
    }
    return *t != '\0' && re_atom_match(p, *t) && re_here(next, t + 1);
}

static bool re_search(const char* pat, const char* text) {
    const char* p = pat;
    for (;;) {
        const char* q = p;
        while (!re_branch_end(q)) {
            q = re_atom_next(q);
        }
        bool anchored = *p == '^';
        const char* body = anchored ? p + 1 : p;
        for (const char* t = text;; t++) {
            if (re_here(body, t)) {
                return true;
            }
            if (anchored || *t == '\0') {
                break;
            }
        }
        if (*q != '|') {
            return false;
        }
        p = q + 1;
    }
}

static bool re_valid(const char* pat) {
    for (const char* p = pat; *p != '\0'; p = re_atom_next(p)) {
        if (*p == '(' || *p == ')' || *p == '{' || *p == '}') {
            return false;
        }
        if (*p == '[' && re_atom_next(p)[-1] != ']') {
            return false;
        }
        if (*p == '\\' && p[1] == '\0') {
            return false;
        }
    }
    return true;
}

static uint64_t now_ms(void) { return svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000); }

static const char* key(void) {
    const char* k = du_cfg()->queue_padfile_key;
    return k != NULL ? k : "padfile";
}

static bool path_of(char* out, size_t n, const char* rel) { return du_files_path(out, n, rel); }

/* Splits a copy of a queue.txt line into the seven fields (in place). */
static bool split(char* line, char* f[FIELDS]) {
    for (int i = 0; i < FIELDS; i++) {
        f[i] = line;
        char* t = strchr(line, '\t');
        if (i == FIELDS - 1) {
            return t == NULL;
        }
        if (t == NULL) {
            return false;
        }
        *t = '\0';
        line = t + 1;
    }
    return false;
}

static bool name_ok(const char* s) {
    size_t n = strlen(s);
    if (n == 0 || n > NAME_MAX_LEN) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
              c == '-')) {
            return false;
        }
    }
    return true;
}

/* queue.txt: counts the entries and, if want >= 1, copies that line into out. */
static int read_entries(int want, char* out, size_t cap) {
    char path[160];
    if (!path_of(path, sizeof(path), "queue.txt")) {
        return 0;
    }
    size_t n = 0;
    char* text = du_store_read(path, &n);
    if (text == NULL) {
        return 0;
    }
    int count = 0;
    for (char* p = text; *p != '\0';) {
        char* e = strchr(p, '\n');
        size_t len = e != NULL ? (size_t)(e - p) : strlen(p);
        if (len > 0 && p[len - 1] == '\r') {
            len--;
        }
        if (len > 0) {
            count++;
            if (count == want && out != NULL && len < cap) {
                memcpy(out, p, len);
                out[len] = '\0';
            }
        }
        if (e == NULL) {
            break;
        }
        p = e + 1;
    }
    free(text);
    return count;
}

static bool load_entry(int n, char* line, size_t cap, Entry* e) {
    line[0] = '\0';
    int total = read_entries(n, line, cap);
    if (n < 1 || n > total || line[0] == '\0') {
        return false;
    }
    char* f[FIELDS];
    if (!split(line, f) || !name_ok(f[0])) {
        return false;
    }
    snprintf(e->name, sizeof(e->name), "%s", f[0]);
    e->sw = f[1];
    e->pad = f[2];
    e->until = f[3];
    e->frames = (uint32_t)strtoul(f[4], NULL, 10);
    e->timeout_s = (uint32_t)strtoul(f[5], NULL, 10);
    if (e->timeout_s == 0) {
        e->timeout_s = DEFAULT_TIMEOUT_S;
    }
    e->collect = f[6];
    return true;
}

/* The deliver-argument text for entry n. */
static bool entry_args(int n, char* out, size_t cap) {
    char line[LINE_MAX_LEN + 1];
    Entry e;
    if (!load_entry(n, line, sizeof(line), &e)) {
        return false;
    }
    int w = snprintf(out, cap, "%s", e.sw);
    if (e.pad[0] != '\0' && w >= 0 && (size_t)w < cap) {
        w += snprintf(out + w, cap - (size_t)w, "%s%s=%s/%s", w > 0 ? " " : "", key(), du_cfg()->files_dir, e.pad);
    }
    if (w >= 0 && (size_t)w < cap) {
        w += snprintf(out + w, cap - (size_t)w, "%sctrdkq=%d", w > 0 ? " " : "", n);
    }
    return w > 0 && (size_t)w < cap;
}

static void write_state(const char* state, int index, int total, int run) {
    char path[160], text[96];
    if (!path_of(path, sizeof(path), "state.txt")) {
        return;
    }
    int n = snprintf(text, sizeof(text), "state=%s\nindex=%d\ntotal=%d\nrun=%d\n", state, index, total, run);
    du_store_write(path, 0, text, (size_t)n);
}

static int read_run(void) {
    char path[160];
    if (!path_of(path, sizeof(path), "state.txt")) {
        return 0;
    }
    size_t n = 0;
    char* text = du_store_read(path, &n);
    int run = 0;
    if (text != NULL) {
        const char* r = strstr(text, "run=");
        run = r != NULL ? atoi(r + 4) : 0;
        free(text);
    }
    return run;
}

/* ---- Running an entry ---- */

void du_queue_init(const char* launch_args) {
    s_n = 0;
    if (!du_files_enabled()) {
        return;
    }
    const char* q = strstr(launch_args, "ctrdkq=");
    if (q == NULL || (q != launch_args && q[-1] != ' ')) {
        return;
    }
    int n = atoi(q + 7);
    if (n < 1 || !load_entry(n, s_line, sizeof(s_line), &s_e)) {
        du_log(0, "[QUEUE] entry %d: not found in queue.txt, running as a normal launch", n);
        return;
    }
    s_total = read_entries(0, NULL, 0);
    s_run = read_run();
    s_t0 = now_ms();
    s_frames = 0;
    s_fin = 0;
    s_re_ok = s_e.until[0] != '\0' && re_valid(s_e.until);
    if (s_e.until[0] != '\0' && !s_re_ok) {
        du_log(0, "[QUEUE] entry %d %s: bad until pattern, ignored", n, s_e.name);
    }
    s_n = n;
    du_log(2, "[QUEUE] entry %d of %d %s start (timeout %lu s, frames %lu)", n, s_total, s_e.name,
           (unsigned long)s_e.timeout_s, (unsigned long)s_e.frames);
}

bool devupdate_queue_active(void) { return s_n != 0; }
int devupdate_queue_entry(void) { return s_n; }

static void pend(const char* result) {
    int expect = 0;
    if (__atomic_compare_exchange_n(&s_fin, &expect, -1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        snprintf(s_result, sizeof(s_result), "%s", result);
        __atomic_store_n(&s_fin, 1, __ATOMIC_RELEASE);
    }
}

void devupdate_queue_frame(void) {
    if (s_n != 0 && s_fin == 0) {
        s_frames++;
        if (s_e.frames != 0 && s_frames >= s_e.frames) {
            pend("frames");
        }
    }
}

void devupdate_queue_log_line(const char* line) {
    if (s_n != 0 && s_fin == 0 && s_re_ok && re_search(s_e.until, line)) {
        pend("pass");
    }
}

void devupdate_queue_finish(const char* result) {
    if (s_n != 0) {
        pend(result != NULL && result[0] != '\0' ? result : "done");
    }
}

static void oneline(char* s) {
    for (; *s != '\0'; s++) {
        if (*s == '\t' || *s == '\n' || *s == '\r') {
            *s = ' ';
        }
    }
}

/* Records the end of the running entry, sets up the next launch, and (recovery) leaves
 * or (otherwise) hands over to the app's relaunch. */
static void complete(const char* result, const char* detail, bool recovery, const char* report) {
    s_fin = 2;
    uint32_t ms = (uint32_t)(now_ms() - s_t0);
    char outdir[96], path[200], line[320], d[110];
    snprintf(d, sizeof(d), "%s", detail);
    oneline(d);
    du_store_flush(); /* what the app queued (shots, saves) must be on the card before it is moved */
    snprintf(outdir, sizeof(outdir), "r%d_%d_%s", s_run, s_n, s_e.name);
    char rel[160];
    snprintf(rel, sizeof(rel), "out/%s", outdir);
    du_files_mkdirs("out/x"); /* files_dir and files_dir/out */
    if (path_of(path, sizeof(path), rel)) {
        du_store_mkdir(path);
    }
    /* collect */
    char coll[LINE_MAX_LEN];
    snprintf(coll, sizeof(coll), "%s", s_e.collect);
    for (char* tok = strtok(coll, ","); tok != NULL; tok = strtok(NULL, ",")) {
        if (!du_rel_ok(tok)) {
            continue;
        }
        const char* base = strrchr(tok, '/');
        base = base != NULL ? base + 1 : tok;
        char dst[200];
        snprintf(dst, sizeof(dst), "%s/%s/%s", du_cfg()->files_dir, rel, base);
        du_store_rename(tok, dst);
    }
    if (report != NULL) {
        snprintf(rel, sizeof(rel), "out/%s/crash.txt", outdir);
        if (path_of(path, sizeof(path), rel)) {
            du_store_write(path, 0, report, strlen(report));
        }
    }
    int w = snprintf(line, sizeof(line), "%d\t%s\t%s\t%lu\t%lu\t%s\t%s\n", s_n, s_e.name, result, (unsigned long)s_frames,
                     (unsigned long)ms, outdir, d);
    if (path_of(path, sizeof(path), "results.txt") && w > 0) {
        du_store_append(path, line, (size_t)(w < (int)sizeof(line) ? w : (int)sizeof(line) - 1));
    }
    du_store_flush();
    du_log(2, "[QUEUE] entry %d %s done result %s frames %lu ms %lu", s_n, s_e.name, result, (unsigned long)s_frames,
           (unsigned long)ms);

    char args[DEVUPDATE_MAX_ARGS + 1] = "";
    int next = s_n + 1;
    if (next <= s_total && entry_args(next, args, sizeof(args))) {
        write_state("running", next, s_total, s_run);
    } else {
        args[0] = '\0';
        write_state("done", s_total, s_total, s_run);
        du_log(2, "[QUEUE] done: %d entries, results in %s/results.txt", s_total, du_cfg()->files_dir);
    }
    du_store_flush();
    svcSleepThread(1200000000LL); /* the log writer sends the lines above to the PC */
    if (recovery) {
        du_set_next(true, 0, args);
        du_recovery_exit(true);
    }
    du_begin(false);
    du_hand_over(-1, true, 0, args, false);
}

void du_queue_tick(void) {
    if (s_n == 0 || s_fin == 2) {
        return;
    }
    if (s_fin == 0 && now_ms() - s_t0 > (uint64_t)s_e.timeout_s * 1000) {
        pend("timeout");
    }
    if (s_fin == 1) {
        complete(s_result, "", false, NULL);
    }
}

bool du_queue_recovery(const char* report) {
    if (s_n == 0) {
        return false;
    }
    if (s_fin == 2) {
        du_recovery_exit(true); /* the next launch is already set */
    }
    char first[110] = "";
    if (report != NULL) {
        const char* r = report + strspn(report, " \r\n\t"); /* the report starts with a newline */
        snprintf(first, sizeof(first), "%.*s", (int)strcspn(r, "\r\n"), r);
    }
    complete("crash", first, true, report);
    return true; /* not reached */
}

/* ---- The queue command ---- */

static void send_long(int fd, const char* prefix, const char* text) {
    du_send_all(fd, prefix, strlen(prefix));
    du_send_all(fd, text, strlen(text));
    du_send_all(fd, "\n", 1);
}

static void q_add(int fd, uint32_t size, uint32_t crc) {
    if (size == 0 || size > 65536) {
        du_send_line(fd, "error 12 queue entries must be 1 to 65536 bytes");
        return;
    }
    char* text = malloc(size + 1);
    if (text == NULL) {
        du_send_line(fd, "error 22 out of memory");
        return;
    }
    du_send_line(fd, "ok receiving %lu", (unsigned long)size);
    uint32_t c = 0xFFFFFFFFu, done = 0;
    while (done < size) {
        int r = du_recv_some(fd, text + done, size - done, 15000);
        if (r <= 0) {
            free(text);
            du_send_line(fd, "error 15 receive failed after %lu bytes", (unsigned long)done);
            return;
        }
        c = du_crc_update(c, (uint8_t*)text + done, (uint32_t)r);
        done += (uint32_t)r;
    }
    text[size] = '\0';
    if (~c != crc) {
        free(text);
        du_send_line(fd, "error 5 crc mismatch");
        return;
    }
    /* validate every line before storing any */
    int lines = 0;
    for (char* p = text; *p != '\0';) {
        char* e = strchr(p, '\n');
        size_t len = e != NULL ? (size_t)(e - p) : strlen(p);
        if (len > 0) {
            char copy[LINE_MAX_LEN + 1];
            char* f[FIELDS];
            if (len > LINE_MAX_LEN) {
                free(text);
                du_send_line(fd, "error 23 entry line %d is longer than %d bytes", lines + 1, LINE_MAX_LEN);
                return;
            }
            memcpy(copy, p, len);
            copy[len] = '\0';
            if (len > 0 && copy[len - 1] == '\r') {
                copy[len - 1] = '\0';
            }
            if (!split(copy, f) || !name_ok(f[0])) {
                free(text);
                du_send_line(fd, "error 23 entry line %d is malformed (7 tab-separated fields, a plain name)", lines + 1);
                return;
            }
            if (f[2][0] != '\0' && !du_rel_ok(f[2])) {
                free(text);
                du_send_line(fd, "error 23 entry %s: bad padfile name", f[0]);
                return;
            }
            if (f[3][0] != '\0' && !re_valid(f[3])) {
                free(text);
                du_send_line(fd, "error 23 entry %s: the until pattern uses groups or counts, which are not supported",
                             f[0]);
                return;
            }
            lines++;
        }
        if (e == NULL) {
            break;
        }
        p = e + 1;
    }
    char path[160];
    if (lines == 0 || !path_of(path, sizeof(path), "queue.txt")) {
        free(text);
        du_send_line(fd, "error 23 no entries");
        return;
    }
    du_store_mkdir(du_cfg()->files_dir);
    int rc = du_store_append(path, text, size);
    if (rc == 0 && text[size - 1] != '\n') {
        rc = du_store_append(path, "\n", 1);
    }
    du_store_flush();
    free(text);
    if (rc != 0) {
        du_send_line(fd, "error 17 cannot write queue.txt");
        return;
    }
    du_send_line(fd, "queued %d total %d", lines, read_entries(0, NULL, 0));
    du_send_line(fd, "done");
}

static void q_list(int fd) {
    char path[160];
    path_of(path, sizeof(path), "queue.txt");
    size_t n = 0;
    char* text = du_store_read(path, &n);
    int i = 0;
    if (text != NULL) {
        for (char* p = text; *p != '\0';) {
            char* e = strchr(p, '\n');
            if (e != NULL) {
                *e = '\0';
            }
            if (p[0] != '\0') {
                char pre[16];
                snprintf(pre, sizeof(pre), "q %d\t", ++i);
                send_long(fd, pre, p);
            }
            if (e == NULL) {
                break;
            }
            p = e + 1;
        }
        free(text);
    }
    du_send_line(fd, "entries %d", i);
    du_send_line(fd, "done");
}

static void q_clear(int fd) {
    char path[160];
    static const char* const files[] = {"queue.txt", "results.txt", "state.txt"};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        if (path_of(path, sizeof(path), files[i])) {
            du_store_remove(path);
        }
    }
    du_store_flush();
    du_send_line(fd, "cleared (out/ is kept)");
    du_send_line(fd, "done");
}

static void q_status(int fd) {
    char path[160];
    path_of(path, sizeof(path), "state.txt");
    size_t n = 0;
    char* text = du_store_read(path, &n);
    char st[120] = "state=idle";
    if (text != NULL) {
        snprintf(st, sizeof(st), "%s", text);
        free(text);
        for (char* c = st; *c != '\0'; c++) {
            if (*c == '\n') {
                *c = ' ';
            }
        }
    }
    path_of(path, sizeof(path), "results.txt");
    int results = 0;
    text = du_store_read(path, &n);
    if (text != NULL) {
        for (char* c = text; *c != '\0'; c++) {
            results += *c == '\n';
        }
        free(text);
    }
    du_send_line(fd, "queue %s results=%d entries=%d launch_entry=%d launch_ms=%lu", st, results, read_entries(0, NULL, 0),
                 s_n, s_n != 0 ? (unsigned long)(now_ms() - s_t0) : 0ul);
    du_send_line(fd, "done");
}

static void q_start(int fd, bool recovery) {
    int total = read_entries(0, NULL, 0);
    char args[DEVUPDATE_MAX_ARGS + 1];
    char path[160];
    if (total == 0) {
        du_send_line(fd, "error 24 the queue is empty (queue add first)");
        return;
    }
    for (int i = 1; i <= total; i++) {
        if (!entry_args(i, args, sizeof(args))) {
            du_send_line(fd, "error 25 entry %d is unreadable or its switches are longer than %d bytes", i,
                         DEVUPDATE_MAX_ARGS);
            return;
        }
    }
    entry_args(1, args, sizeof(args));
    int run = read_run() + 1;
    if (path_of(path, sizeof(path), "results.txt")) {
        du_store_remove(path);
    }
    write_state("running", 1, total, run);
    du_store_flush();
    du_log(2, "[QUEUE] start: %d entries, run %d", total, run);
    du_send_line(fd, "ok starting %d entries (run %d)", total, run);
    du_begin(recovery);
    du_hand_over(fd, true, 0, args, recovery);
}

void du_cmd_queue(int fd, const char* args, uint32_t size, uint32_t crc, bool recovery) {
    if (!du_files_enabled()) {
        du_send_line(fd, "error 19 this app has no files folder");
        return;
    }
    if (strcmp(args, "add") == 0) {
        q_add(fd, size, crc);
    } else if (strcmp(args, "list") == 0) {
        q_list(fd);
    } else if (strcmp(args, "clear") == 0) {
        q_clear(fd);
    } else if (strcmp(args, "start") == 0) {
        q_start(fd, recovery);
    } else if (strcmp(args, "status") == 0) {
        q_status(fd);
    } else {
        du_send_line(fd, "error 3 unknown queue command %s", args);
    }
}
