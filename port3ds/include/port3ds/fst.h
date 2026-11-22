/* GameCube disc file system table: parsing and path lookup (port3ds/game/fst.c).
 *
 * Pure: no threads, no I/O, no platform calls. The DVD layer (game/dvd.c) and the host decode tool
 * (tools/port3ds/host_decode) both use it. */
#ifndef PORT3DS_FST_H
#define PORT3DS_FST_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t name;   /* offset in the string table */
    uint32_t offset; /* file: disc offset; dir: parent index */
    uint32_t size;   /* file: size; dir: index after the last child */
    uint8_t is_dir;
} PortFstEntry;

typedef struct {
    PortFstEntry* entries;
    const char* names; /* inside the buffer given to port_fst_load */
    uint32_t count;
} PortFst;

/* Parses the raw FST (`size` bytes at `raw`, big-endian, as on the disc). `raw` must stay valid: the string
 * table is read from it. Returns 0, or -1 when the table is damaged. */
int port_fst_load(PortFst* fst, const uint8_t* raw, uint32_t size);
/* Entry number of a path (relative to the root; "." and ".." allowed; names compared without case), or -1. */
int32_t port_fst_find(const PortFst* fst, const char* path);
/* Full path of entry i ("a/a_mario"; directories have no trailing slash). */
void port_fst_path(const PortFst* fst, uint32_t i, char* out, size_t n);

#endif
