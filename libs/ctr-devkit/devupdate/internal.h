/* ctr-devkit devupdate: what devupdate.c, files.c and queue.c share. Not
 * part of the public API.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) the ctr-devkit authors
 */
#ifndef CTRDK_DEVUPDATE_INTERNAL_H
#define CTRDK_DEVUPDATE_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ctrdk/devupdate.h"

#define DU_CHUNK 0x10000
#define DU_MAX_FILE (16u << 20)

/* devupdate.c */
const DevUpdateConfig* du_cfg(void);
void du_log(int level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void du_send_line(int fd, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void du_send_all(int fd, const char* s, size_t n);
int du_recv_some(int fd, void* buf, size_t n, int timeout_ms); /* >0 got, 0 closed, -1 timeout, -2 sleep */
uint8_t* du_buf(void);                                          /* DU_CHUNK static bytes, server thread only */
uint32_t du_crc_update(uint32_t crc, const uint8_t* p, uint32_t n); /* start from 0xFFFFFFFF, finish with ~ */
void du_set_next(bool self, uint64_t tid, const char* args);
void du_begin(bool recovery);
void du_hand_over(int fd, bool self, uint64_t tid, const char* args, bool recovery); /* fd < 0: no replies */
void du_recovery_exit(bool relaunch) __attribute__((noreturn));
bool du_relaunch_requested(void); /* the relaunch of this title is under way */

/* files.c: the store (callbacks or stdio) and the files command. */
bool du_files_enabled(void);
bool du_files_ro_enabled(void);  /* files_ro_dir is set */
bool du_files_ro_path(char* out, size_t n, const char* rel); /* files_ro_dir + "/" + rel, as du_files_path */
void du_files_use_direct(bool direct); /* recovery: the stdio store, whatever the app passed */
int du_store_write(const char* path, uint64_t off, const void* data, size_t n);
int du_store_append(const char* path, const void* data, size_t n);
int du_store_rename(const char* from, const char* to);
int du_store_remove(const char* path);
int du_store_mkdir(const char* path);
void du_store_flush(void);
void* du_store_read(const char* path, size_t* n);
/* files_dir + "/" + rel; false if rel is not a safe relative path or it does not fit */
bool du_files_path(char* out, size_t n, const char* rel);
bool du_rel_ok(const char* rel);
void du_files_mkdirs(const char* rel_file); /* the folders above files_dir/rel_file */
void du_cmd_files(int fd, const char* args, uint32_t size, uint32_t crc);

/* queue.c */
void du_queue_init(const char* launch_args); /* from devupdate_start: reads ctrdkq=N, loads the entry */
void du_cmd_queue(int fd, const char* args, uint32_t size, uint32_t crc, bool recovery);
void du_queue_tick(void);                    /* server thread, a few times a second: ends an entry that is due */
bool du_queue_recovery(const char* report);  /* recovery thread: a queue run records the crash and relaunches (does not return then) */

#endif
