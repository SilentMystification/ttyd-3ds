/* Map files ('m/<map>/d', mapdrv.c _mapLoad).
 *
 * Layout: a 0x20-byte header, the data, then the relocation table (offsets
 * in the data of every pointer word), the chunk table ({data offset, name
 * offset}), the extern table and the chunk names. The game makes the
 * pointer words absolute itself (mapLoadReloc_); this converts the file to
 * host order before that:
 *
 *   1. the header and the three tables;
 *   2. every word the relocation table lists (pointers, data-relative), at
 *      any alignment (two stub maps have their material at data +0x47);
 *   3. each chunk by name, walking its structs (paper-cut's
 *      port/bedecode/map.spec): the joint tree, parts, materials and their
 *      samplers and textures, meshes, lights, fog, the texture and material
 *      name tables, the vertex arrays and the animation table.
 *
 * Only words the relocation table lists are pointers; a non-zero word
 * elsewhere in a pointer field is NULL to the game, as on the GameCube where
 * it was never patched, and is not followed.
 *
 * Vertex arrays are converted to host order for the GX layer: f32
 * positions, normals and texture coordinates in older files, s16 positions
 * and texture coordinates and s8 normals in "ver1.02" files (mapSetPolygon).
 * Colours are bytes. Packed display lists stay in GameCube byte order: the
 * GX layer decodes them so, and hitdrv reads their counts and indices
 * big-endian (HIT_DL_U16). Texture data (MapTexInfo.data) is built at run
 * time; material TEV mode bytes are bytes. */
#include <dolphin/os.h>
#include <stdlib.h>
#include <string.h>

#include "plat/plat.h"
#include "port_formats.h"
#include "port_swap_gen.h"

#define DATA 0x20u /* data start in the file */

typedef struct {
    PortSwap s;
    u8* reloc; /* one bit per byte: a pointer word starts here */
    int ver102;
} MapSwap;

static int is_reloc(const MapSwap* m, u32 off) { return off < m->s.size && (m->reloc[off >> 3] >> (off & 7) & 1); }

/* The file offset a pointer field at file offset `field` points to, or 0. */
static u32 ptr(const MapSwap* m, u32 field) {
    if (!is_reloc(m, field)) {
        return 0;
    }
    u32 v = port_swap_get32(&m->s, field);
    return v != 0 ? DATA + v : 0;
}

static void walk_joint(MapSwap* m, u32 off);
static void walk_mesh(MapSwap* m, u32 off);

static void walk_tex(MapSwap* m, u32 off) {
    if (off != 0 && port_swap_visit(&m->s, off)) {
        port_swap_struct(&m->s, off, &port_swap_type_MapTexInfo);
    }
}

static void walk_material(MapSwap* m, u32 off) {
    if (off == 0 || !port_swap_visit(&m->s, off)) {
        return;
    }
    port_swap_struct(&m->s, off, &port_swap_type_MapMaterial);
    for (u32 i = 0; i < 8; i++) {
        u32 smp = ptr(m, off + 0xC + i * 4);
        if (smp != 0 && port_swap_visit(&m->s, smp)) {
            port_swap_struct(&m->s, smp, &port_swap_type_MapMatSampler);
            walk_tex(m, ptr(m, smp));
        }
    }
}

/* A vertex array: u32 count, then the elements. kind: 0 positions,
 * 1 normals, 2 colours, 3 texture coordinates. */
static void walk_array(MapSwap* m, u32 off, int kind) {
    if (off == 0 || !port_swap_visit(&m->s, off)) {
        return;
    }
    port_swap_units(&m->s, off, 1, 4);
    u32 n = port_swap_get32(&m->s, off);
    u32 data = off + 4;
    switch (kind) {
    case 0:
        port_swap_units(&m->s, data, n * 3, m->ver102 ? 2 : 4);
        break;
    case 1:
        if (!m->ver102) {
            port_swap_units(&m->s, data, n * 3, 4); /* (ver1.02: s8) */
        }
        break;
    case 2: /* RGBA8 bytes */
        break;
    case 3:
        port_swap_units(&m->s, data, n * 2, m->ver102 ? 2 : 4);
        break;
    }
}

static void walk_vcd(MapSwap* m, u32 off) {
    if (off == 0 || !port_swap_visit(&m->s, off)) {
        return;
    }
    port_swap_struct(&m->s, off, &port_swap_type_MapVcdTable);
    walk_array(m, ptr(m, off + 0x0), 0);
    walk_array(m, ptr(m, off + 0x4), 1);
    for (u32 i = 0; i < 2; i++) {
        walk_array(m, ptr(m, off + 0xC + i * 4), 2);
    }
    for (u32 i = 0; i < 8; i++) {
        walk_array(m, ptr(m, off + 0x18 + i * 4), 3);
    }
}

static void walk_mesh(MapSwap* m, u32 off) {
    if (off == 0 || !port_swap_visit(&m->s, off)) {
        return;
    }
    port_swap_struct(&m->s, off, &port_swap_type_MapFileMesh);
    int packed = m->s.base[off + 3] != 0;
    u32 n = port_swap_get32(&m->s, off + 4);
    walk_vcd(m, ptr(m, off + 0xC));
    for (u32 i = 0; i < n; i++) {
        if (packed) {
            /* {offset (a pointer), size}; the list itself stays big-endian */
            port_swap_units(&m->s, off + 0x10 + i * 8 + 4, 1, 4);
        } else {
            u32 dl = ptr(m, off + 0x10 + i * 4);
            if (dl != 0 && port_swap_visit(&m->s, dl)) {
                port_swap_units(&m->s, dl, 1, 4);
                port_swap_array(&m->s, dl + 4, port_swap_get32(&m->s, dl), &port_swap_type_MapVtxRec);
            }
        }
    }
}

static void walk_joint(MapSwap* m, u32 off) {
    while (off != 0 && port_swap_visit(&m->s, off)) {
        port_swap_struct(&m->s, off, &port_swap_type_MapFileJoint);
        u32 dm = ptr(m, off + 0x58);
        if (dm != 0 && port_swap_visit(&m->s, dm)) {
            port_swap_struct(&m->s, dm, &port_swap_type_MapJointDrawMode);
        }
        u32 parts = port_swap_get32(&m->s, off + 0x5C);
        port_swap_array(&m->s, off + 0x60, parts, &port_swap_type_MapFileJointPart);
        for (u32 i = 0; i < parts; i++) {
            walk_material(m, ptr(m, off + 0x60 + i * 8));
            walk_mesh(m, ptr(m, off + 0x60 + i * 8 + 4));
        }
        walk_joint(m, ptr(m, off + 0xC)); /* child */
        off = ptr(m, off + 0x10);         /* next sibling (a loop: sibling chains are long) */
    }
}

/* Animation data: every field is a 4-byte word (pointers, counts, floats,
 * key steps). */
static void walk_anim_item(MapSwap* m, u32 off, u32 head_words, u32 count_word, u32 key_words) {
    if (off == 0 || !port_swap_visit(&m->s, off)) {
        return;
    }
    port_swap_units(&m->s, off, head_words, 4);
    u32 keys = port_swap_get32(&m->s, off + count_word * 4);
    port_swap_units(&m->s, off + head_words * 4, keys * key_words, 4);
}

static void walk_anim_list(MapSwap* m, u32 off, u32 head_words, u32 count_word, u32 key_words) {
    if (off == 0 || !port_swap_visit(&m->s, off)) {
        return;
    }
    port_swap_units(&m->s, off, 1, 4);
    u32 n = port_swap_get32(&m->s, off);
    port_swap_units(&m->s, off + 4, n, 4);
    for (u32 i = 0; i < n; i++) {
        walk_anim_item(m, ptr(m, off + 4 + i * 4), head_words, count_word, key_words);
    }
}

static void walk_animation_table(MapSwap* m, u32 off) {
    port_swap_units(&m->s, off, 1, 4);
    u32 n = port_swap_get32(&m->s, off);
    port_swap_units(&m->s, off + 4, n, 4);
    for (u32 i = 0; i < n; i++) {
        u32 h = ptr(m, off + 4 + i * 4);
        if (h == 0 || !port_swap_visit(&m->s, h)) {
            continue;
        }
        port_swap_struct(&m->s, h, &port_swap_type_MapAnimHeader);
        port_swap_units(&m->s, h + 4, 1, 4); /* pad_04: a word of the file, never read */
        /* key: 1 time word + channels x 5 words (MapAnimKeyCh) */
        walk_anim_list(m, ptr(m, h + 0xC), 0x5C / 4, 0x58 / 4, 1 + 21 * 5); /* MapAnimJoint */
        walk_anim_list(m, ptr(m, h + 0x10), 5, 4, 1 + 5 * 5);               /* MapAnimMatData */
        walk_anim_list(m, ptr(m, h + 0x14), 2, 1, 1 + 4 * 5);               /* MapAnimAlphaData */
        walk_anim_list(m, ptr(m, h + 0x18), 2, 1, 1 + 9 * 5);               /* MapAnimLightData */
        walk_anim_list(m, ptr(m, h + 0x1C), 2, 1, 1 + 5 * 5);               /* MapAnimLightData2 */
    }
}

static const char* chunk_name(const MapSwap* m, u32 str_off, u32 name) {
    u32 at = str_off + name;
    if (at >= m->s.size || memchr(m->s.base + at, 0, m->s.size - at) == NULL) {
        port_swap_bad(&m->s, "chunk name", at, 1);
    }
    return (const char*)m->s.base + at;
}

void portMapSwap(void* data, u32 size, const char* name) {
    u8* p = data;
    if (p == NULL || size < DATA) {
        OSPanic("map.c", __LINE__, "%s: a map file of %lu bytes", name, (unsigned long)size);
    }
    u32 native0;
    memcpy(&native0, p, 4);
    if (port_be32(p) != size) {
        if (native0 == size) {
            return; /* converted already (a map archive's file opened again) */
        }
        OSPanic("map.c", __LINE__, "%s: header says %lu bytes, the file has %lu", name, (unsigned long)port_be32(p),
                (unsigned long)size);
    }
    MapSwap m;
    memset(&m, 0, sizeof(m));
    port_swap_begin(&m.s, data, size, name);
    m.reloc = calloc(size / 8 + 1, 1);
    if (m.reloc == NULL) {
        plat_fatal("%s: no memory to convert the map", name);
    }

    port_swap_struct(&m.s, 0, &port_swap_type_MapFileHeader);
    u32 data_size = port_swap_get32(&m.s, 4), rel_count = port_swap_get32(&m.s, 8);
    u32 chunk_count = port_swap_get32(&m.s, 0xC), extern_count = port_swap_get32(&m.s, 0x10);
    u32 rel_off = DATA + data_size;
    u32 chunk_off = rel_off + rel_count * 4;
    u32 extern_off = chunk_off + chunk_count * 8;
    u32 str_off = extern_off + extern_count * 8;
    if (data_size > size - DATA || rel_count > size / 4 || chunk_count > size / 8 || extern_count > size / 8 ||
        str_off > size) {
        port_swap_bad(&m.s, "header tables", rel_off, str_off - rel_off);
    }
    port_swap_units(&m.s, rel_off, rel_count, 4);
    port_swap_array(&m.s, chunk_off, chunk_count, &port_swap_type_MapFileChunk);
    port_swap_array(&m.s, extern_off, extern_count, &port_swap_type_MapFileChunk);

    /* 2. pointer words */
    for (u32 k = 0; k < rel_count; k++) {
        u32 r = port_swap_get32(&m.s, rel_off + k * 4);
        if (r > data_size || data_size - r < 4) {
            port_swap_bad(&m.s, "relocation", DATA + r, 4);
        }
        port_swap_word_any(&m.s, DATA + r);
        m.reloc[(DATA + r) >> 3] |= (u8)(1u << ((DATA + r) & 7));
    }

    /* 3. chunks: "information" first, for the vertex formats */
    u32 info = 0;
    for (u32 k = 0; k < chunk_count; k++) {
        if (!strcmp(chunk_name(&m, str_off, port_swap_get32(&m.s, chunk_off + k * 8 + 4)), "information")) {
            info = DATA + port_swap_get32(&m.s, chunk_off + k * 8);
        }
    }
    if (info == 0) {
        OSPanic("map.c", __LINE__, "%s: no information chunk", name);
    }
    port_swap_struct(&m.s, info, &port_swap_type_MapFileInfo);
    u32 version = ptr(&m, info);
    m.ver102 = version != 0 && version + 8 <= size && !strcmp((const char*)p + version, "ver1.02");

    for (u32 k = 0; k < chunk_count; k++) {
        const char* cname = chunk_name(&m, str_off, port_swap_get32(&m.s, chunk_off + k * 8 + 4));
        u32 off = DATA + port_swap_get32(&m.s, chunk_off + k * 8);
        if (off >= size) {
            port_swap_bad(&m.s, cname, off, 1);
        }
        if (!strcmp(cname, "information")) {
            walk_joint(&m, ptr(&m, info + 4));
        } else if (!strcmp(cname, "texture_table")) {
            port_swap_struct(&m.s, off, &port_swap_type_MapTexTable); /* the names are pointers */
        } else if (!strcmp(cname, "material_name_table")) {
            port_swap_struct(&m.s, off, &port_swap_type_MapMaterialNameTable);
            u32 n = port_swap_get32(&m.s, off);
            port_swap_array(&m.s, off + 4, n, &port_swap_type_MapMaterialName);
            for (u32 i = 0; i < n; i++) {
                walk_material(&m, ptr(&m, off + 4 + i * 8 + 4));
            }
        } else if (!strcmp(cname, "light_table")) {
            port_swap_struct(&m.s, off, &port_swap_type_MapFileLightTable);
            u32 n = port_swap_get32(&m.s, off);
            for (u32 i = 0; i < n; i++) {
                u32 l = ptr(&m, off + 4 + i * 4);
                if (l != 0 && port_swap_visit(&m.s, l)) {
                    port_swap_struct(&m.s, l, &port_swap_type_MapFileLight);
                }
            }
        } else if (!strcmp(cname, "fog_table")) {
            port_swap_struct(&m.s, off, &port_swap_type_MapFileFog);
        } else if (!strcmp(cname, "vcd_table")) {
            walk_vcd(&m, off);
        } else if (!strcmp(cname, "animation_table")) {
            walk_animation_table(&m, off);
        } else {
            PLOG_W("%s: chunk \"%s\" left as it is (no reader in the game)", name, cname);
        }
    }
    PLOG_D("map %s: %lu bytes, %lu pointers, %lu chunks%s", name, (unsigned long)size, (unsigned long)rel_count,
           (unsigned long)chunk_count, m.ver102 ? ", ver1.02" : "");
    free(m.reloc);
    port_swap_end(&m.s);
}
