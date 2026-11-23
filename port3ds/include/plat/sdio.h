/* SD card service (plat/sdio.c): all SD access at run time happens on core 1.
 *
 * One worker thread on core 1 (lowest priority but the log's) runs a queue of
 * file operations. Writers hand it an owned copy of their bytes and go on
 * without waiting; threads on cores 0 and 2 never block on SD I/O (only on a
 * full queue, which is bounded in requests and bytes). Disc image reads have
 * their own thread on core 1 (plat/disc.c) and report here for sdstats.
 *
 * Paths are relative to the working directory (sdmc:/3ds/ttyd). Game
 * agnostic: nothing here knows the game. Types are fixed-size (plat.h rule).
 *
 * Switch sdstats=N: every N seconds one log line with the operations by kind
 * and by the core that asked, the queue depth, and the SD calls made directly
 * (outside this service) from core 0 or 2 after boot, with the first
 * caller's address. Direct calls are seen through the linker's --wrap of
 * fopen/open/opendir/remove/rename/mkdir/unlink/fsync/stat (port3ds/CMakeLists.txt). */
#ifndef PLAT_SDIO_H
#define PLAT_SDIO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PLAT_SD_FSYNC = 1,  /* fsync before close */
    PLAT_SD_ATOMIC = 2, /* write_file: to <path>.tmp, then replace <path> (a torn write never shows) */
};

/* Start the worker. Before it, requests run on the calling thread (boot). */
int plat_sd_start(void);
/* The game loop has begun: direct SD calls from cores 0 and 2 count from here on. */
void plat_sd_boot_done(void);
/* A crash report is being written: direct calls are exempt from now on. */
void plat_sd_crashed(void);

/* Async writes: the bytes are copied (or, with _own, taken: a malloc'd block
 * the service frees). Return 0 queued, -1 refused (no memory, bad path). The
 * call waits only while the queue is full. Failures are logged by the worker. */
int plat_sd_write_file(const char* path, const void* data, size_t n, uint32_t flags);
int plat_sd_write_file_own(const char* path, void* data, size_t n, uint32_t flags);
int plat_sd_append(const char* path, const void* data, size_t n);
int plat_sd_write_at(const char* path, uint64_t offset, const void* data, size_t n); /* creates the file if absent */
int plat_sd_remove(const char* path);
int plat_sd_rename(const char* from, const char* to);
int plat_sd_mkdir(const char* path);
/* Waits until everything queued before the call has run. */
void plat_sd_flush(void);
/* Sync read of a whole file (boot, or a caller that blocks by design): a
 * malloc'd block with a NUL after the n bytes, or NULL if the file is absent. */
void* plat_sd_read_file(const char* path, size_t* n);

/* The core the calling thread was created for (as sdstats sees it); the syscall's answer for threads made elsewhere. */
int plat_sd_core(void);

/* plat/disc.c: a disc read ran for a request from `req_core`. */
void plat_sd_note_disc(int req_core, uint32_t bytes);

#ifdef __cplusplus
}
#endif

#endif
