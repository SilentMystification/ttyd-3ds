/* Camera roads ('m/<map>/c', cam_road.c camRoadSetup).
 *
 * After paper-cut's port/src/common/camroad_pc.c. The header holds eight
 * counts (0xC8..0xE7: curves first) and nine table offsets (0xE8..0x10B);
 * the tables are 32-bit words (floats, s32 indices and offsets), except the
 * curve names (bytes 0..0x1F of each 0xB8-byte curve) and the group pads
 * (bytes 0..0x3F of each 0x68-byte group), which stay bytes. An empty
 * table's offset word is stale and is not followed. The last table is never
 * read by the game and stays as it is.
 *
 * The header's first bytes are padding the game never reads; the port
 * writes a marker there so a file opened again from a map archive is not
 * converted twice. */
#include <dolphin/os.h>
#include <string.h>

#include "plat/plat.h"
#include "port_formats.h"

#define HEADER 0x10C
#define CURVE_SIZE 0xB8
#define CURVE_NAME 0x20
#define GROUP_SIZE 0x68
#define GROUP_PAD 0x40

enum { T_PROJECTION, T_CURVES, T_GROUPS, T_STRIPS, T_CURVE_VTX, T_GROUP_VTX, T_STRIP_IDX, T_COLL_GROUPS, T_EXTRA, T_COUNT };

static const u8 k_marker[8] = {'3', 'D', 'S', 'C', 'A', 'M', 'R', 0};

static void swap_records(PortSwap* s, u32 off, u32 len, u32 stride, u32 from, u32 count) {
    if (len / stride < count) {
        count = len / stride;
    }
    for (u32 i = 0; i < count; i++) {
        port_swap_units(s, off + i * stride + from, (stride - from) / 4, 4);
    }
}

void portCamRoadSwap(void* data, u32 size, const char* name) {
    u8* b = data;
    if (b == NULL || size < HEADER) {
        OSPanic("camroad.c", __LINE__, "%s: a camera road of %lu bytes", name, (unsigned long)size);
    }
    if (memcmp(b, k_marker, sizeof(k_marker)) == 0) {
        return; /* converted already */
    }
    PortSwap s;
    port_swap_begin(&s, data, size, name);
    u32 off[T_COUNT], end[T_COUNT];
    for (u32 i = 0; i < T_COUNT; i++) {
        off[i] = port_be32(b + 0xE8 + 4 * i);
        if (i > T_PROJECTION && port_be32(b + 0xC8 + 4 * (i - T_CURVES)) == 0) {
            off[i] = 0; /* empty table: the offset word is stale */
        }
        if (off[i] > size || (off[i] != 0 && off[i] < HEADER)) {
            port_swap_bad(&s, "table", off[i], 0);
        }
    }
    for (u32 i = 0; i < T_COUNT; i++) {
        end[i] = size;
        for (u32 j = 0; j < T_COUNT; j++) {
            if (off[j] > off[i] && off[j] < end[i]) {
                end[i] = off[j];
            }
        }
    }
    u32 curves = port_be32(b + 0xC8);
    port_swap_units(&s, 0xC8, (HEADER - 0xC8) / 4, 4);
    for (u32 i = 0; i < T_COUNT; i++) {
        if (off[i] == 0) {
            continue;
        }
        u32 len = end[i] - off[i];
        switch (i) {
        case T_CURVES: swap_records(&s, off[i], len, CURVE_SIZE, CURVE_NAME, curves); break;
        case T_GROUPS:
        case T_COLL_GROUPS: swap_records(&s, off[i], len, GROUP_SIZE, GROUP_PAD, len / GROUP_SIZE); break;
        case T_EXTRA: break;
        default: port_swap_units(&s, off[i], len / 4, 4); break;
        }
    }
    memcpy(b, k_marker, sizeof(k_marker));
    PLOG_D("camroad %s: %lu curves", name, (unsigned long)curves);
    port_swap_end(&s);
}
