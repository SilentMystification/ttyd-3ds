/* ctr-devkit devupdate: header and argument-area decoding (see proto.h).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) the ctr-devkit authors
 */
#include <string.h>

#include "proto.h"

int du_proto_check_header(const DuHeader* h, int max_args) {
    if (memcmp(h->magic, DU_MAGIC, 4) != 0 || h->version != DU_PROTO_VERSION) {
        return 1;
    }
    if (h->args_len > (uint32_t)max_args) {
        return 3;
    }
    return 0;
}

int du_proto_split_args(char* args, uint32_t args_len, uint32_t flags, char* incoming, size_t incoming_n) {
    incoming[0] = '\0';
    args[args_len] = '\0';
    if (!(flags & DU_FLAG_VERSION)) {
        return (int)args_len;
    }
    if (args_len < 1u || args_len < 1u + (uint8_t)args[0]) {
        return -1; /* the length byte promises more than the area holds */
    }
    uint32_t n = (uint8_t)args[0];
    uint32_t keep = n < incoming_n - 1 ? n : (uint32_t)(incoming_n - 1);
    memcpy(incoming, args + 1, keep);
    incoming[keep] = '\0';
    uint32_t rest = args_len - 1 - n; /* the switches follow the version text, whatever its length */
    memmove(args, args + 1 + n, rest);
    args[rest] = '\0';
    return (int)rest;
}
