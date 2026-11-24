/* The port's own resource files (plat/res.c): data the port needs that is neither code nor game data, for example
 * a generated font. They ship next to the binary (the release package's res/ folder) and live on the SD card in
 * sdmc:/3ds/ttyd/res/, never inside the CIA or 3DSX. Game agnostic: names are the caller's.
 *
 * Lookup order, relative to the working directory (sdmc:/3ds/ttyd):
 *   res/<name>         where the user copies the package's res/ folder
 *   queue/res/<name>   where `devupdate.py files put <file> --dest res` leaves a file (the devupdate files folder)
 *
 * The read goes through the SD service (plat/sdio.h), so any thread may call it; the first call of a name reads
 * the file, later calls return the same block. */
#ifndef PLAT_RES_H
#define PLAT_RES_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The resource's bytes (a block that stays valid for the life of the process), size in *n; NULL if the file is
 * absent or empty. Not thread-safe against another first call of the same name: call from the boot path. */
const void* plat_res_get(const char* name, size_t* n);

/* Text for a "missing resource" message: "sdmc:/3ds/ttyd/res/<name>". */
const char* plat_res_path(const char* name);

#ifdef __cplusplus
}
#endif

#endif
