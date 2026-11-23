/* ctr-devkit devupdate: the wire header and its decoding. Plain C with no libctru, so the PC can
 * build it with gcc (devupdate.py selftest) and feed it the packets the PC tool really sends.
 * Not part of the public API.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) the ctr-devkit authors
 */
#ifndef CTRDK_DEVUPDATE_PROTO_H
#define CTRDK_DEVUPDATE_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DU_MAGIC "CDU1"
#define DU_PROTO_VERSION 1
#define DU_VERSION_MAX 47 /* longest incoming version text kept (a longer one is cut, not refused) */

enum { DU_CMD_PING = 1, DU_CMD_PUSH, DU_CMD_RESTART, DU_CMD_REPORT, DU_CMD_LAUNCH, DU_CMD_FILES, DU_CMD_QUEUE };
enum {
    DU_FLAG_RELAUNCH = 1u << 0,
    DU_FLAG_LAUNCH = 1u << 1,
    DU_FLAG_VERSION = 1u << 2, /* the argument area starts with one length byte and that many characters */
    DU_FLAG_STAGE = 1u << 3    /* push: write the CIA to the SD card first, install from there */
};

typedef struct __attribute__((packed)) {
    char magic[4];
    uint16_t version;
    uint16_t cmd;
    uint32_t flags;
    uint32_t size;
    uint32_t crc;
    uint32_t args_len;
    uint64_t title_id;
    char token[32];
} DuHeader;
_Static_assert(sizeof(DuHeader) == 64, "protocol header is 64 bytes");

/* 0 = fine, else the error number the device sends ("error 1 ..."). */
int du_proto_check_header(const DuHeader* h, int max_args);

/* The argument area (args_len bytes, in args, which has room for args_len + 1): when flags has
 * DU_FLAG_VERSION, the leading length byte and text are taken off into incoming (NUL ended, at most
 * incoming_n - 1 characters; a longer text is cut) and the switch text moves to the start of args.
 * args is NUL ended afterwards. Returns the switch text length, or -1 if the version text does not
 * fit in args_len (a malformed packet: the whole area is then switch text and incoming is ""). */
int du_proto_split_args(char* args, uint32_t args_len, uint32_t flags, char* incoming, size_t incoming_n);

#endif
