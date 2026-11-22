/* GameCube disc file system table: see port3ds/fst.h. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port3ds/fst.h"

static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

int port_fst_load(PortFst* fst, const uint8_t* raw, uint32_t size) {
    if (size < 12) {
        return -1;
    }
    uint32_t count = be32(raw + 8);
    if (count == 0 || (uint64_t)count * 12 > size) {
        return -1;
    }
    PortFstEntry* e = malloc(sizeof(PortFstEntry) * count);
    if (e == NULL) {
        return -1;
    }
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* r = raw + i * 12;
        uint32_t w0 = be32(r);
        e[i].is_dir = (uint8_t)(w0 >> 24);
        e[i].name = w0 & 0xFFFFFF;
        e[i].offset = be32(r + 4);
        e[i].size = be32(r + 8);
    }
    fst->entries = e;
    fst->names = (const char*)raw + (size_t)count * 12;
    fst->count = count;
    return 0;
}

static int name_eq(const char* fst_name, const char* path, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (fst_name[i] == '\0' || tolower((unsigned char)fst_name[i]) != tolower((unsigned char)path[i])) {
            return 0;
        }
    }
    return fst_name[len] == '\0';
}

int32_t port_fst_find(const PortFst* fst, const char* path) {
    const PortFstEntry* f = fst->entries;
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
            dir = f[dir].offset;
        } else {
            uint32_t i = dir + 1;
            uint32_t last = f[dir].size;
            uint32_t found = 0;
            while (i < last) {
                if (name_eq(fst->names + f[i].name, p, len)) {
                    found = i;
                    break;
                }
                i = f[i].is_dir ? f[i].size : i + 1;
            }
            if (found == 0) {
                return -1;
            }
            if (!f[found].is_dir) {
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

void port_fst_path(const PortFst* fst, uint32_t i, char* out, size_t n) {
    const PortFstEntry* f = fst->entries;
    uint32_t chain[64];
    int depth = 0;
    out[0] = '\0';
    if (i == 0 || i >= fst->count) {
        return;
    }
    uint32_t e = i;
    chain[depth++] = e;
    while (depth < 64) {
        /* the parent is the closest earlier directory whose range covers e */
        uint32_t parent = 0;
        for (uint32_t j = e; j-- > 0;) {
            if (f[j].is_dir && (j == 0 || f[j].size > e)) {
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
        int w = snprintf(out + len, n - len, "%s%s", len ? "/" : "", fst->names + f[chain[k]].name);
        if (w < 0 || (size_t)w >= n - len) {
            break;
        }
        len += (size_t)w;
    }
}
