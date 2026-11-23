/* Host build of the device's request decoder, for `devupdate.py selftest`: reads one request
 * (header, argument area and as much payload as follows) from stdin and decodes it with proto.c, the
 * same code the console runs. Prints one key=value per line; the PC side compares them with what it
 * meant to send, including the offset where the payload starts.
 *
 *   gcc -O1 -Wall -o selftest_host selftest_host.c proto.c
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) the ctr-devkit authors
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proto.h"

#define MAX_ARGS 0x2F0

static void print_hex(const char* key, const char* s) {
    printf("%s=", key);
    for (; *s != '\0'; s++) {
        printf("%02x", (unsigned char)*s);
    }
    printf("\n");
}

int main(void) {
    static unsigned char in[1 << 16];
    size_t n = fread(in, 1, sizeof(in), stdin);
    if (n < sizeof(DuHeader)) {
        printf("error=short header %zu\n", n);
        return 1;
    }
    DuHeader h;
    memcpy(&h, in, sizeof(h));
    int bad = du_proto_check_header(&h, MAX_ARGS);
    if (bad != 0) {
        printf("error=%d\n", bad);
        return 1;
    }
    if (sizeof(h) + h.args_len > n) {
        printf("error=short args\n");
        return 1;
    }
    char args[MAX_ARGS + 1];
    char incoming[DU_VERSION_MAX + 1];
    memcpy(args, in + sizeof(h), h.args_len);
    int sw = du_proto_split_args(args, h.args_len, h.flags, incoming, sizeof(incoming));
    printf("cmd=%u\nflags=%u\nsize=%u\ncrc=%08x\ntitle=%016llx\nargs_len=%u\n", h.cmd, h.flags, h.size, h.crc,
           (unsigned long long)h.title_id, h.args_len);
    printf("payload_offset=%zu\n", sizeof(h) + h.args_len);
    printf("malformed=%d\n", sw < 0);
    print_hex("version_hex", incoming);
    print_hex("switches_hex", args);
    return 0;
}
