/* GameCube disc image: header check and the FST, for the host.
 *
 * The FST parsing and path lookup are port3ds/game/fst.c, the code dvd.c uses; this file adds the disc header
 * check (as DVDInit does) and the three port_dvd_* calls that disc_data.c makes. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_decode.h"
#include "port3ds/fst.h"

int host_disc_attach(FILE* f);

static FILE* s_file;
static char s_id[7];
static uint32_t s_dol;
static PortFst s_tab;

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
    if (port_fst_load(&s_tab, raw, fst_size) != 0) {
        snprintf(err, err_n, "the file table is damaged");
        return -1;
    }
    return 0;
}

const char* host_disc_id(void) { return s_id; }
uint32_t host_fst_count(void) { return s_tab.count; }
const FstEntry* host_fst_entry(uint32_t i) { return i < s_tab.count ? (const FstEntry*)&s_tab.entries[i] : NULL; }
void host_fst_path(uint32_t i, char* out, size_t n) { port_fst_path(&s_tab, i, out, n); }
int32_t host_fst_find(const char* path) { return port_fst_find(&s_tab, path); }

/* ---- what port3ds/game/disc_data.c asks of the DVD layer (dvd.c) ---- */

int32_t port_dvd_read_raw(uint32_t offset, void* buf, uint32_t size) { return host_disc_read(offset, buf, size); }
uint32_t port_dvd_dol_offset(void) { return s_dol; }
uint32_t port_dvd_raw_file(const char* path, uint32_t* size) {
    int32_t e = host_fst_find(path);
    if (e <= 0 || s_tab.entries[e].is_dir) {
        return 0;
    }
    *size = s_tab.entries[e].size;
    return s_tab.entries[e].offset;
}
