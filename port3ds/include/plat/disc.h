/* Disc image access for the game side (implemented in plat/disc.c).
 *
 * The user's disc image is sdmc:/3ds/ttyd/game.iso. Reads go straight to
 * the SD card through the 3DS file system service (no stdio buffering).
 * Asynchronous reads run on a file thread on core 1 and call `done` there,
 * in FIFO order. */
#ifndef PLAT_DISC_H
#define PLAT_DISC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PLAT_DISC_PATH "/3ds/ttyd/game.iso"

/* Returns 0 and the image size, or a negative error (no file, not readable). */
int plat_disc_open(uint64_t* size_out);
/* Synchronous read; returns the bytes read or a negative error. */
int32_t plat_disc_read(uint64_t offset, void* buf, uint32_t size);
typedef void (*PlatDiscDone)(void* ctx, int32_t result);
/* Queues a read; returns 0, or a negative error when the queue is full. */
int plat_disc_read_async(uint64_t offset, void* buf, uint32_t size, PlatDiscDone done, void* ctx);
/* Reads done since boot and bytes moved (HUD, profiler). */
void plat_disc_stats(uint32_t* reads, uint64_t* bytes);

#ifdef __cplusplus
}
#endif

#endif
