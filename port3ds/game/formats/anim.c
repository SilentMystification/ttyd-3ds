/* Animated models ('a/<name>' files, filemgr type 5, animdrv.c).
 *
 * The pointer graph follows paper-cut's port/bedecode/anim.spec and
 * port/src/common/anim_pc.c. Converted to host order:
 *   - the CPU-read tables (shapes, draw calls, draws, groups, texture
 *     entries and binds, texture matrices, the animation table);
 *   - the float arrays: vertex positions and normals, node data (a run of
 *     bufferNodeNum floats), texture coordinates (the GX layer reads arrays
 *     in host order);
 *   - the 32-bit index tables (posIndex, nrmIndex, clr0Index, texIndex[]);
 *   - each AnimData (its pointers are relative to the AnimData itself): its
 *     header, loop info, keyframes, texture deltas and value keys.
 * Left as bytes: vertex colours, group visibility, the vertex, normal,
 * visibility and node delta streams (byte entries).
 *
 * Counts the decomp's header keeps in padding (no access found in the
 * game) are read at their offsets; paper-cut names them. A model is
 * recognised as unconverted by its size word (offset 0), which is the
 * file's own size in disc order. */
#include <dolphin/os.h>

#include "plat/plat.h"
#include "port_formats.h"
#include "port_swap_gen.h"

#define POSE_HEADER 0x1B0

/* Offsets of AnimationPoseData fields, in the disc layout. */
enum {
    P_SHAPE_COUNT = 0xE8,
    P_DRAWCALL_COUNT = 0xEC, /* in pad_EC */
    P_POS_NUM = 0xF0,
    P_POS_IDX_NUM = 0xF4,
    P_NRM_NUM = 0xF8,
    P_NRM_IDX_NUM = 0xFC,
    P_CLR0_NUM = 0x100, /* 0x100..0x12B in pad_100 */
    P_CLR0_IDX_NUM = 0x104,
    P_TEX_IDX_NUM = 0x108, /* [8] */
    P_TEX_VTX_NUM = 0x128,
    P_TEXANIM_NUM = 0x12C,
    P_TEXDATA_COUNT = 0x130,
    P_TEXBIND_COUNT = 0x134,
    P_DRAW_COUNT = 0x138, /* in pad_138 */
    P_NODE_NUM = 0x140,
    P_GROUP_COUNT = 0x144,
    P_ANIM_COUNT = 0x148,
    P_SHAPES = 0x14C,
    P_DRAWCALLS = 0x150,
    P_VTX_POS = 0x154,
    P_POS_INDEX = 0x158,
    P_NRM_VTX = 0x15C,
    P_NRM_INDEX = 0x160,
    P_CLR0_INDEX = 0x168,
    P_TEX_INDEX = 0x16C, /* [8] */
    P_TEX_VTX = 0x18C,
    P_TEXANIM = 0x190,
    P_TEXDATA = 0x194,
    P_TEXBINDS = 0x198,
    P_DRAWS = 0x19C,
    P_NODES = 0x1A4,
    P_GROUPS = 0x1A8,
    P_ANIMS = 0x1AC,
};

/* AnimData, relative to its own start. */
enum {
    A_SIZE = 0x0, /* 0x0..0x7 in pad_00 */
    A_LOOP_COUNT = 0x4,
    A_KEYFRAME_COUNT = 0x8,
    A_TEXDELTA_COUNT = 0x14, /* 0xC..0x1F in pad_0C */
    A_VALUEKEY_COUNT = 0x20,
    A_LOOPINFO = 0x24,
    A_KEYFRAMES = 0x28,
    A_TEXDELTAS = 0x34,
    A_VALUEKEYS = 0x40,
};

static void swap_table(PortSwap* s, u32 ptr_field, u32 count, const PortSwapType* t) {
    u32 off = port_swap_get32(s, ptr_field);
    if (count != 0 && off != 0) {
        port_swap_array(s, off, count, t);
    }
}

static void swap_words(PortSwap* s, u32 ptr_field, u32 count) {
    u32 off = port_swap_get32(s, ptr_field);
    if (count != 0 && off != 0) {
        port_swap_units(s, off, count, 4);
    }
}

/* One AnimData at file offset base (its pointers are relative to base).
 * Converted within the model's PortSwap, so data shared between entries is
 * converted once. */
static void swap_anim_data(PortSwap* s, u32 base) {
    if (!port_swap_in(s, base, 0x5C)) {
        port_swap_bad(s, "AnimData", base, 0x5C);
    }
    port_swap_struct(s, base, &port_swap_type_AnimData);
    port_swap_units(s, base + A_SIZE, 2, 4);
    port_swap_units(s, base + 0xC, 5, 4); /* vtxPos/vtxNrm/tex/vis/node delta counts */
    u32 loops = port_swap_get32(s, base + A_LOOP_COUNT);
    u32 off = port_swap_get32(s, base + A_LOOPINFO);
    if (off != 0) {
        port_swap_units(s, base + off, (loops != 0 ? loops : 1) * 3, 4);
    }
    off = port_swap_get32(s, base + A_KEYFRAMES);
    u32 n = port_swap_get32(s, base + A_KEYFRAME_COUNT);
    if (off != 0 && n != 0) {
        port_swap_array(s, base + off, n, &port_swap_type_AnimKeyframe);
    }
    off = port_swap_get32(s, base + A_TEXDELTAS);
    n = port_swap_get32(s, base + A_TEXDELTA_COUNT);
    if (off != 0 && n != 0) {
        port_swap_array(s, base + off, n, &port_swap_type_AnimTexDelta);
    }
    off = port_swap_get32(s, base + A_VALUEKEYS);
    n = port_swap_get32(s, base + A_VALUEKEY_COUNT);
    if (off != 0 && n != 0) {
        port_swap_array(s, base + off, n, &port_swap_type_AnimValueKey);
    }
}

void portAnimSwap(void* data, u32 size, const char* name) {
    u8* p = data;
    if (p == NULL || size < POSE_HEADER) {
        OSPanic("anim.c", __LINE__, "%s: an animation model of %lu bytes", name, (unsigned long)size);
    }
    /* The size word is the file's size: in disc order until converted. */
    u32 be_size = port_be32(p);
    if (be_size < POSE_HEADER || be_size > size) {
        PLOG_D("anim %s: size word %08lx (file %lu bytes): converted already", name, (unsigned long)port_be32(p),
               (unsigned long)size);
        return;
    }
    PortSwap s;
    port_swap_begin(&s, data, size, name);
    port_swap_struct(&s, 0, &port_swap_type_AnimationPoseData);
    port_swap_units(&s, P_DRAWCALL_COUNT, 1, 4);
    port_swap_units(&s, P_CLR0_NUM, 11, 4);
    port_swap_units(&s, P_DRAW_COUNT, 1, 4);

    swap_table(&s, P_SHAPES, port_swap_get32(&s, P_SHAPE_COUNT), &port_swap_type_AnimPoseShape);
    swap_table(&s, P_DRAWCALLS, port_swap_get32(&s, P_DRAWCALL_COUNT), &port_swap_type_AnimDrawCallEntry);
    swap_words(&s, P_VTX_POS, port_swap_get32(&s, P_POS_NUM) * 3);
    swap_words(&s, P_POS_INDEX, port_swap_get32(&s, P_POS_IDX_NUM));
    swap_words(&s, P_NRM_VTX, port_swap_get32(&s, P_NRM_NUM) * 3);
    swap_words(&s, P_NRM_INDEX, port_swap_get32(&s, P_NRM_IDX_NUM));
    swap_words(&s, P_CLR0_INDEX, port_swap_get32(&s, P_CLR0_IDX_NUM));
    for (u32 i = 0; i < 8; i++) {
        /* An unused slot has a stale offset: only counted slots are followed. */
        swap_words(&s, P_TEX_INDEX + i * 4, port_swap_get32(&s, P_TEX_IDX_NUM + i * 4));
    }
    swap_words(&s, P_TEX_VTX, port_swap_get32(&s, P_TEX_VTX_NUM) * 2);
    swap_table(&s, P_TEXANIM, port_swap_get32(&s, P_TEXANIM_NUM), &port_swap_type_AnimTexMtx);
    swap_table(&s, P_TEXDATA, port_swap_get32(&s, P_TEXDATA_COUNT), &port_swap_type_AnimPoseTexEntry);
    swap_table(&s, P_TEXBINDS, port_swap_get32(&s, P_TEXBIND_COUNT), &port_swap_type_AnimTexBind);
    swap_table(&s, P_DRAWS, port_swap_get32(&s, P_DRAW_COUNT), &port_swap_type_AnimPoseShapeDraw);
    swap_words(&s, P_NODES, port_swap_get32(&s, P_NODE_NUM));
    swap_table(&s, P_GROUPS, port_swap_get32(&s, P_GROUP_COUNT), &port_swap_type_AnimPoseGroup);

    u32 count = port_swap_get32(&s, P_ANIM_COUNT);
    u32 table = port_swap_get32(&s, P_ANIMS);
    if (count != 0 && table != 0) {
        port_swap_array(&s, table, count, &port_swap_type_AnimTableEntry);
        for (u32 i = 0; i < count; i++) {
            u32 off = port_swap_get32(&s, table + i * port_swap_type_AnimTableEntry.size + 0x3C);
            if (off != 0) {
                swap_anim_data(&s, off);
            }
        }
    }
    PLOG_D("anim %s: %lu bytes, %lu shapes, %lu positions, %lu nodes, %lu anims", name, (unsigned long)size,
           (unsigned long)port_swap_get32(&s, P_SHAPE_COUNT), (unsigned long)port_swap_get32(&s, P_POS_NUM),
           (unsigned long)port_swap_get32(&s, P_NODE_NUM), (unsigned long)count);
    port_swap_end(&s);
}
