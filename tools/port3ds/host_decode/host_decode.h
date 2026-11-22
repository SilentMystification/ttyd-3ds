/* Host decode tool: the interface between the tool (main.c, fst.c, shim.c) and a game's converters.
 *
 * Nothing in this header, nor in main.c, fst.c and shim.c, knows what a game's formats are. A game supplies one
 * file with the converter table (formats_ttyd.c for TTYD); the tool decodes every file of the disc image that
 * a table row matches, with the converter sources the port itself is built from. */
#ifndef HOST_DECODE_H
#define HOST_DECODE_H

#include <stddef.h>
#include <stdint.h>

/* ---- the game's converter table (formats_<game>.c) ---- */

typedef struct HostFormat {
    const char* name;                     /* label in reports */
    int (*match)(const char* path);       /* nonzero when the disc file `path` is this format */
    void (*convert)(void* data, uint32_t size, const char* path); /* the port's converter, as the game calls it */
    uint32_t read_pad;                    /* the game reads the file rounded up to this many bytes (0: exact) */
    uint32_t read_max;                    /* the converter is given only this many leading bytes (0: the whole file) */
    uint32_t size_arg;                    /* if nonzero, passed as the size instead of the buffer's (UNKNOWN sizes) */
    int idempotent;                       /* a second call on converted data must change nothing */
} HostFormat;

extern const HostFormat host_formats[];
extern const int host_format_count;

/* ---- the disc (fst.c) ---- */

typedef struct {
    uint32_t name;   /* offset in the string table */
    uint32_t offset; /* file: disc offset. dir: parent index */
    uint32_t size;   /* file: size. dir: index after the last child */
    uint8_t is_dir;
} FstEntry; /* the layout of PortFstEntry (port3ds/fst.h) */

/* Opens the image, checks it is a GameCube image, loads the FST. Returns 0, or -1 with a message in err. */
int host_disc_open(const char* path, char* err, size_t err_n);
const char* host_disc_id(void); /* the 6-character game ID */
uint32_t host_fst_count(void);
const FstEntry* host_fst_entry(uint32_t i);
/* Full path of entry i ("a/a_mario", directories without a trailing slash). */
void host_fst_path(uint32_t i, char* out, size_t n);
/* Entry number of a path (case-insensitive, as the game resolves paths), or -1. */
int32_t host_fst_find(const char* path);
/* Reads n bytes at an absolute disc offset; returns n, or a negative error. */
int32_t host_disc_read(uint64_t offset, void* buf, uint32_t n);

/* ---- failures (shim.c) ----
 * The converters stop on a bad file with plat_fatal() or OSPanic(): on the 3DS that ends the process. Here the
 * call returns to the caller of host_guard_run with the message. */
typedef void (*HostGuardFn)(void* arg);
/* Runs fn(arg); returns 0, or 1 with the message in msg when it stopped through plat_fatal/OSPanic. */
int host_guard_run(HostGuardFn fn, void* arg, char* msg, size_t msg_n);
void host_set_log_level(int level);

#endif
