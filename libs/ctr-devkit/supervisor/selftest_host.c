/* Host build of the supervisor's policy (policy.c, the same file the console runs), for
 * `devupdate.py selftest`. Reads one command per line from stdin and prints one result line each:
 *
 *   init M                 sv_wd_init(max_relaunches M)            -> "ok"
 *   arm S T                sv_wd_arm(S seconds) at T ms            -> "ok"
 *   launched T / pushed T                                          -> "ok"
 *   hb FRAME FLAGS T       sv_wd_heartbeat                         -> "ok"
 *   tick GAME T            sv_wd_tick (GAME -1, 0, 1)              -> "tick=R relaunches=N watch=S"
 *   kv ARGS|KEY|DEF        sv_kv_int                               -> "kv=V"
 *   word ARGS|WORD         sv_has_word                             -> "word=0|1"
 *   largs SWITCHES|N       sv_launch_args_text into N bytes        -> "largs=LEN text"
 *   rel PATH               sv_rel_ok                               -> "rel=0|1"
 *
 *   gcc -O1 -Wall -o sv_selftest selftest_host.c policy.c
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) the ctr-devkit authors
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "policy.h"

static char* field(char** p) {
    char* s = *p;
    char* bar = strchr(s, '|');
    if (bar != NULL) {
        *bar = '\0';
        *p = bar + 1;
    } else {
        *p = s + strlen(s);
    }
    return s;
}

int main(void) {
    SvWatch w;
    sv_wd_init(&w, 2);
    char line[1024];
    while (fgets(line, sizeof(line), stdin) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        char* rest = strchr(line, ' ');
        if (rest != NULL) {
            *rest++ = '\0';
        } else {
            rest = line + strlen(line);
        }
        unsigned long long a = 0, b = 0, c = 0;
        if (strcmp(line, "init") == 0) {
            sv_wd_init(&w, (uint32_t)strtoul(rest, NULL, 0));
            puts("ok");
        } else if (strcmp(line, "arm") == 0 && sscanf(rest, "%llu %llu", &a, &b) == 2) {
            sv_wd_arm(&w, (uint32_t)a, b);
            puts("ok");
        } else if (strcmp(line, "launched") == 0) {
            sv_wd_launched(&w, strtoull(rest, NULL, 0));
            puts("ok");
        } else if (strcmp(line, "pushed") == 0) {
            sv_wd_pushed(&w, strtoull(rest, NULL, 0));
            puts("ok");
        } else if (strcmp(line, "hb") == 0 && sscanf(rest, "%llu %llu %llu", &a, &b, &c) == 3) {
            sv_wd_heartbeat(&w, (uint32_t)a, (uint32_t)b, c);
            puts("ok");
        } else if (strcmp(line, "tick") == 0) {
            long long g = 0;
            if (sscanf(rest, "%lld %llu", &g, &b) != 2) {
                puts("error=bad tick");
                continue;
            }
            int r = sv_wd_tick(&w, (int)g, b);
            printf("tick=%d relaunches=%u watch=%u\n", r, (unsigned)w.relaunches, (unsigned)w.watch_s);
        } else if (strcmp(line, "kv") == 0) {
            char* p = rest;
            char* args = field(&p);
            char* key = field(&p);
            long def = strtol(field(&p), NULL, 0);
            printf("kv=%ld\n", sv_kv_int(args, key, def));
        } else if (strcmp(line, "word") == 0) {
            char* p = rest;
            char* args = field(&p);
            printf("word=%d\n", sv_has_word(args, field(&p)) ? 1 : 0);
        } else if (strcmp(line, "largs") == 0) {
            char* p = rest;
            char* sw = field(&p);
            size_t n = (size_t)strtoul(field(&p), NULL, 0);
            char out[1024];
            if (n > sizeof(out)) {
                n = sizeof(out);
            }
            int len = sv_launch_args_text(out, n, sw);
            printf("largs=%d %s\n", len, out);
        } else if (strcmp(line, "rel") == 0) {
            printf("rel=%d\n", sv_rel_ok(rest) ? 1 : 0);
        } else {
            printf("error=unknown %s\n", line);
        }
        fflush(stdout);
    }
    return 0;
}
