/* GameCube disc image: header check and file system table, for the host.
 *
 * Reads the FST as port3ds/game/dvd.c (DVDInit, DVDConvertPathToEntrynum) does, with the same checks and the same
 * case-insensitive path walk. That code is built on the 3DS OS layer (threads, interrupts, the DVD manager), so
 * it cannot be compiled here; the two are kept alike by hand and tools/port3ds/test_decode.py compares this
 * listing with paper-cut's gcdisc.py. Also provides the three port_dvd_* calls that disc_data.c makes. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_decode.h"

int host_disc_attach(FILE* f);

static FILE* s_file;
static char s_id[7];
static uint32_t s_dol;
static FstEntry* s_fst;
static char* s_names;
static uint32_t s_count;

static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

int host_disc_open(const char* path, char* err, size_t err_n) {
    s_file = fopen(path, "rb");
    if (s_file == NULL) {
        snprintf(err, err_n, "cannot open %s", path);
        return -1;
    }
    host_disc_attach(s_file);
    uint8_t header[0x440];
    if (host_disc_read(0, header, sizeof(header)) != (int32_t)sizeof(header)) {
        snprintf(err, err_n, "%s: shorter than a GameCube disc header", path);
        return -1;
    }
    if (be32(header + 0x1C) != 0xC2339F3Du) {
        snprintf(err, err_n, "%s: not a plain GameCube disc image (convert .rvz/.ciso to .iso first)", path);
        return -1;
    }
    memcpy(s_id, header, 6);
    for (int i = 0; i < 6; i++) {
        s_id[i] = isprint((unsigned char)s_id[i]) ? s_id[i] : '?';
    }
    s_id[6] = '\0';
    s_dol = be32(header + 0x420);
    uint32_t fst_offset = be32(header + 0x424), fst_size = be32(header + 0x428);
    uint8_t* raw = malloc(fst_size + 1);
    if (raw == NULL || host_disc_read(fst_offset, raw, fst_size) != (int32_t)fst_size) {
        snprintf(err, err_n, "the file table cannot be read");
        return -1;
    }
    s_count = be32(raw + 8);
    if (s_count == 0 || (uint64_t)s_count * 12 > fst_size) {
        snprintf(err, err_n, "the file table is damaged");
        return -1;
    }
    s_fst = malloc(sizeof(FstEntry) * s_count);
    for (uint32_t i = 0; i < s_count; i++) {
        const uint8_t* e = raw + i * 12;
        uint32_t w0 = be32(e);
        s_fst[i].is_dir = (uint8_t)(w0 >> 24);
        s_fst[i].name = w0 & 0xFFFFFF;
        s_fst[i].offset = be32(e + 4);
        s_fst[i].size = be32(e + 8);
    }
    raw[fst_size] = '\0';
    s_names = (char*)raw + (size_t)s_count * 12;
    return 0;
}

const char* host_disc_id(void) { return s_id; }
uint32_t host_fst_count(void) { return s_count; }
const FstEntry* host_fst_entry(uint32_t i) { return i < s_count ? &s_fst[i] : NULL; }

void host_fst_path(uint32_t i, char* out, size_t n) {
    /* parents: a directory entry holds its parent's index */
    uint32_t chain[64];
    int depth = 0;
    out[0] = '\0';
    if (i == 0 || i >= s_count) {
        return;
    }
    uint32_t e = i;
    chain[depth++] = e;
    uint32_t parent = 0;
    /* the parent of an entry is the closest earlier directory whose range covers it */
    while (depth < 64) {
        parent = 0;
        for (uint32_t j = e; j-- > 0;) {
            if (s_fst[j].is_dir && (j == 0 || s_fst[j].size > e)) {
                parent = j;
                break;
            }
        }
        if (parent == 0) {
            break;
        }
        chain[depth++] = parent;
        e = parent;
    }
    size_t len = 0;
    for (int k = depth - 1; k >= 0; k--) {
        int w = snprintf(out + len, n - len, "%s%s", len ? "/" : "", s_names + s_fst[chain[k]].name);
        if (w < 0 || (size_t)w >= n - len) {
            break;
        }
        len += (size_t)w;
    }
}

static int name_eq(const char* fst_name, const char* path, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (fst_name[i] == '\0' || tolower((unsigned char)fst_name[i]) != tolower((unsigned char)path[i])) {
            return 0;
        }
    }
    return fst_name[len] == '\0';
}

/* DVDConvertPathToEntrynum, unchanged in logic. */
int32_t host_fst_find(const char* path) {
    uint32_t dir = 0;
    const char* p = path;
    while (*p == '/') {
        p++;
    }
    for (;;) {
        if (*p == '\0') {
            return (int32_t)dir;
        }
        const char* end = p;
        while (*end != '\0' && *end != '/') {
            end++;
        }
        size_t len = (size_t)(end - p);
        if (len == 1 && p[0] == '.') {
            /* stays in dir */
        } else if (len == 2 && p[0] == '.' && p[1] == '.') {
            dir = s_fst[dir].offset;
        } else {
            uint32_t i = dir + 1, last = s_fst[dir].size, found = 0;
            while (i < last) {
                if (name_eq(s_names + s_fst[i].name, p, len)) {
                    found = i;
                    break;
                }
                i = s_fst[i].is_dir ? s_fst[i].size : i + 1;
            }
            if (found == 0) {
                return -1;
            }
            if (!s_fst[found].is_dir) {
                return *end == '\0' ? (int32_t)found : -1;
            }
            dir = found;
        }
        p = end;
        while (*p == '/') {
            p++;
        }
    }
}

/* ---- what port3ds/game/disc_data.c asks of the DVD layer (dvd.c) ---- */

int32_t port_dvd_read_raw(uint32_t offset, void* buf, uint32_t size) { return host_disc_read(offset, buf, size); }
uint32_t port_dvd_dol_offset(void) { return s_dol; }
uint32_t port_dvd_raw_file(const char* path, uint32_t* size) {
    int32_t e = host_fst_find(path);
    if (e <= 0 || s_fst[e].is_dir) {
        return 0;
    }
    *size = s_fst[e].size;
    return s_fst[e].offset;
}
