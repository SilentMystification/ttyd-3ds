/* Data compiled into the decomp that ports read from the disc (include/port_disc_data.h).
 *
 * Game files register their arrays with constructors (PORT_DISC_DATA); the
 * memory card art the matching build extracts from main.dol is defined
 * here. After DVDInit has checked the image, every array is filled from
 * main.dol (by address, through the DOL's section table) or from a REL
 * inside the image (by section and offset, through the REL's section
 * table). A missing or short read is fatal: the image was checked, so it
 * means the image is damaged. */
#include <dolphin/os.h>
#include <string.h>

#include "plat/plat.h"
#include "port_disc_data.h"

s32 port_dvd_read_raw(u32 offset, void* buf, u32 size);
u32 port_dvd_dol_offset(void);
u32 port_dvd_raw_file(const char* path, u32* size);

/* cardmgr.c's memory card icon, palette and banner */
u8 _mariost_icon_tex[0x400] __attribute__((aligned(32)));
u8 _mariost_icon_tlut[0x200] __attribute__((aligned(32)));
u8 _mariost_banner_tex[0x1800] __attribute__((aligned(32)));
PORT_DISC_DATA(_mariost_icon_tex, mariost_icon_tex)
PORT_DISC_DATA(_mariost_icon_tlut, mariost_icon_tlut)
PORT_DISC_DATA(_mariost_banner_tex, mariost_banner_tex)

typedef struct {
    void* dst;
    const char* rel; /* "" for main.dol */
    int section;
    u32 offset; /* main.dol: address */
    u32 size;
    int fmt; /* how the disc's big-endian bytes become memory: see port_disc_data.h */
} Entry;

static Entry s_entries[1024];
static int s_count;

void port_disc_data_register(void* dst, const char* rel, int section, unsigned int offset, unsigned int size, int fmt) {
    if (s_count >= (int)(sizeof(s_entries) / sizeof(s_entries[0]))) {
        plat_fatal("disc data: too many entries");
    }
    s_entries[s_count++] = (Entry){dst, rel, section, offset, size, fmt};
}

static void (*s_fixups[8])(void);
static int s_nfixups;

void port_disc_data_add_fixup(void (*fn)(void)) {
    if (s_nfixups >= (int)(sizeof(s_fixups) / sizeof(s_fixups[0]))) {
        plat_fatal("disc data: too many fixups");
    }
    s_fixups[s_nfixups++] = fn;
}

static u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

static u32 dol_file_offset(u32 addr, u32 size) {
    static u8 h[0x100];
    static u32 dol;
    if (dol == 0) { /* the header is read once */
        dol = port_dvd_dol_offset();
        if (port_dvd_read_raw(dol, h, sizeof(h)) != (s32)sizeof(h)) {
            dol = 0;
            return 0;
        }
    }
    for (int i = 0; i < 18; i++) { /* 7 text, 11 data sections */
        u32 off = be32(h + i * 4), a = be32(h + 0x48 + i * 4), sz = be32(h + 0x90 + i * 4);
        if (sz && addr >= a && addr + size <= a + sz) {
            return dol + off + (addr - a);
        }
    }
    return 0;
}

static u32 rel_file_offset(const char* rel, int section, u32 offset, u32 size) {
    char path[32];
    snprintf(path, sizeof(path), "rel/%s.rel", rel);
    u32 rel_size;
    u32 base = port_dvd_raw_file(path, &rel_size);
    if (base == 0) {
        return 0;
    }
    u8 h[0x40];
    if (port_dvd_read_raw(base, h, sizeof(h)) != (s32)sizeof(h)) {
        return 0;
    }
    u32 nsec = be32(h + 0x0C), secinfo = be32(h + 0x10);
    if (section <= 0 || (u32)section >= nsec) {
        return 0;
    }
    u8 s[8];
    if (port_dvd_read_raw(base + secinfo + (u32)section * 8, s, 8) != 8) {
        return 0;
    }
    u32 soff = be32(s) & ~1u, ssize = be32(s + 4);
    if (soff == 0 || offset + size > ssize || soff + offset + size > rel_size) {
        return 0;
    }
    return base + soff + offset;
}

/* The formats of port_disc_data.h; keep in step with tools/port3ds/disc_geo.py (swap). */
static void swap_bytes(u8* p, u32 size, u32 step) {
    for (u32 i = 0; i + step <= size; i += step) {
        for (u32 a = i, b = i + step - 1; a < b; a++, b--) {
            u8 t = p[a];
            p[a] = p[b];
            p[b] = t;
        }
    }
}

static void swap_entry(u8* p, u32 size, int fmt) {
    switch (fmt) {
        case 1: /* s16 */
            swap_bytes(p, size, 2);
            break;
        case 2: /* f32, u32, Vec */
            swap_bytes(p, size, 4);
            break;
        case 3: /* effN64Vtx: five s16, then r g b a */
            for (u32 v = 0; v + 14 <= size; v += 14) {
                swap_bytes(p + v, 10, 2);
            }
            break;
        default: /* raw bytes, and u32 display lists in GX byte order */
            break;
    }
}

/* The main.dol arrays sit close together in the image, so they are read in windows: entries sorted by file
 * offset, neighbours (gap under 256 bytes) share one read of at most WINDOW bytes. */
#define WINDOW 0x8000
static u8 s_window[WINDOW];
static u32 s_at[1024];    /* an entry's file offset */
static u32 s_order[1024]; /* the DOL entries by file offset */

static void fill_dol_entries(int n) {
    int i = 0;
    while (i < n) {
        const Entry* first = &s_entries[s_order[i]];
        u32 start = s_at[s_order[i]];
        u32 end = start + first->size;
        int j = i + 1;
        while (j < n && s_at[s_order[j]] <= end + 256 &&
               s_at[s_order[j]] + s_entries[s_order[j]].size - start <= WINDOW && end - start <= WINDOW) {
            u32 e2 = s_at[s_order[j]] + s_entries[s_order[j]].size;
            if (e2 > end) {
                end = e2;
            }
            j++;
        }
        if (end - start > WINDOW) { /* a single entry larger than a window: read it straight */
            if (port_dvd_read_raw(start, first->dst, first->size) != (s32)first->size) {
                plat_fatal("disc data: main.dol %08lx not read", (unsigned long)first->offset);
            }
            swap_entry(first->dst, first->size, first->fmt);
            i++;
            continue;
        }
        if (port_dvd_read_raw(start, s_window, end - start) != (s32)(end - start)) {
            plat_fatal("disc data: main.dol window at %08lx not read", (unsigned long)first->offset);
        }
        for (int k = i; k < j; k++) {
            const Entry* e = &s_entries[s_order[k]];
            memcpy(e->dst, s_window + (s_at[s_order[k]] - start), e->size);
            swap_entry(e->dst, e->size, e->fmt);
        }
        i = j;
    }
}

void port_disc_data_fill(void) {
    int n = 0;
    for (int i = 0; i < s_count; i++) {
        const Entry* e = &s_entries[i];
        if (e->rel[0]) {
            u32 where = rel_file_offset(e->rel, e->section, e->offset, e->size);
            if (where == 0 || port_dvd_read_raw(where, e->dst, e->size) != (s32)e->size) {
                plat_fatal("disc data: %s %08lx (%lu bytes) not found in the image", e->rel, (unsigned long)e->offset,
                           (unsigned long)e->size);
            }
            swap_entry(e->dst, e->size, e->fmt);
            continue;
        }
        s_at[i] = dol_file_offset(e->offset, e->size);
        if (s_at[i] == 0) {
            plat_fatal("disc data: main.dol %08lx (%lu bytes) not found in the image", (unsigned long)e->offset,
                       (unsigned long)e->size);
        }
        int k = n++; /* insertion sort by file offset */
        while (k > 0 && s_at[s_order[k - 1]] > s_at[i]) {
            s_order[k] = s_order[k - 1];
            k--;
        }
        s_order[k] = (u32)i;
    }
    fill_dol_entries(n);
    for (int i = 0; i < s_nfixups; i++) {
        s_fixups[i]();
    }
    PLOG_I("disc data: %d arrays read from the image", s_count);
}
