/* Name index for mapGetMapObj, hitNameToPtr and lightNameToPtr (switch mapnameidx; port3ds/include/port3ds/gamepar.h).
 *
 * The game finds a map object, a hit entry or a light by walking every item and comparing names with strcmp; the
 * first match in walk order wins (entry by entry, joint by joint). In gor_01 and pik_00 that walk is 1-3 ms per frame
 * in Azahar (mapObjRotate, mapGrpFlagOn and mapMain call it every frame).
 *
 * The index: one open-addressed table per map group (field 0, battle 1) and kind. A slot holds the top 16 bits of
 * the name's FNV-1a hash and the item number + 1; 0 is empty. Items are inserted in walk order with linear probing,
 * so the items of one name sit along one probe path in walk order: the first candidate that passes the flag test
 * and the strcmp is the walk's answer. Flags (hit DELETED, light ENABLED) are read at lookup time, names compared
 * live, so a flag change needs no rebuild.
 *
 * Validity: a group's table is rebuilt at its next lookup when portNameIdxChanged ran (map load and unload, battle
 * map load and unload, a mobj's hit entries, a light entry, the light reset) or when its key changed (entry count,
 * the object / hit arrays and their lengths, the map file). mapnameidx=2 runs the walk too and counts differences.
 *
 * Hardware fit (New 3DS): a table is 4 bytes a slot at a load factor of at most 1/2 (gor_01: 2 KB to 8 KB), read
 * sequentially from one home slot; the home slot is a mask of the hash (no divide); no syscalls, no doubles. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "driver/hitdrv.h"
#include "driver/lightdrv.h"
#include "driver/mapdrv.h"
#include "plat/plat.h"
#include "port3ds/gamepar.h"

extern MapWork* mapWork; /* mapdrv.c: the field and battle groups */

#define KIND_MAP 0
#define KIND_HIT 1
#define KIND_LIGHT 2
#define MAX_ITEMS 0xFFFEu
#define KEY_WORDS 8

typedef struct {
    u32* slots;
    u32 cap;  /* slots allocated */
    u32 mask; /* slots in use - 1 */
    u32 n0;   /* items of entry 0 (map, hit) */
    u32 key[KEY_WORDS];
    const void* owner; /* the MapWork or LightWork it indexes */
    int valid;         /* 0: rebuild at the next lookup; -1: too many items, walk */
} Idx;

static Idx s_idx[3][2];
static int s_mode = -1; /* mapnameidx: 0 walk, 1 index, 2 index + walk compare */
int port_nameidx_route = -1;

typedef struct {
    u32 lookups, mismatches, rebuilds, probes, compares;
} KindStats;
static KindStats s_stats[3];
static u32 s_logged_mismatch;

int port_nameidx_probe(void) {
    s_mode = plat_switch_int("mapnameidx", 0);
    if (s_mode < 0 || s_mode > 2) {
        s_mode = 0;
    }
    port_nameidx_route = (s_mode > 0 || port_gamecnt_on()) ? 1 : 0;
    if (s_mode > 0) {
        PLOG_I("nameidx: mapnameidx=%d (%s)", s_mode, s_mode == 2 ? "index, walk compared" : "index");
    }
    return port_nameidx_route;
}

void portNameIdxChanged(void) {
    for (int k = 0; k < 3; k++) {
        for (int g = 0; g < 2; g++) {
            if (s_idx[k][g].valid > 0) {
                s_idx[k][g].valid = 0;
            }
            if (s_idx[k][g].valid < 0) {
                s_idx[k][g].valid = 0; /* recount: the new map may fit */
            }
        }
    }
}

static inline u32 name_hash(const char* s) {
    u32 h = 2166136261u;
    for (; *s != '\0'; s++) {
        h ^= (u8)*s;
        h *= 16777619u;
    }
    return h;
}

static inline u32 home_of(u32 h, u32 mask) { return (h ^ (h >> 16)) & mask; }

static int idx_alloc(Idx* x, u32 n) {
    u32 cap = 64;
    while (cap < 2 * n) {
        cap <<= 1;
    }
    if (cap > x->cap) {
        u32* p = realloc(x->slots, cap * sizeof(u32));
        if (p == NULL) {
            return 0;
        }
        x->slots = p;
        x->cap = cap;
    }
    x->mask = cap - 1;
    memset(x->slots, 0, cap * sizeof(u32));
    return 1;
}

static inline void idx_insert(Idx* x, const char* name, u32 item) {
    u32 h = name_hash(name);
    u32 s = home_of(h, x->mask);
    while (x->slots[s] != 0) {
        s = (s + 1) & x->mask;
    }
    x->slots[s] = (h & 0xFFFF0000u) | (item + 1);
}

/* ---- keys ---- */

static int map_group(const MapWork* wp) { return wp == &mapWork[1] ? 1 : 0; }

static inline s32 entry_count(const MapWork* wp) {
    s32 c = wp->entries[0].count;
    return c < 0 ? 0 : c > 2 ? 2 : c;
}

static void map_key(const MapWork* wp, int kind, u32* key) {
    memset(key, 0, KEY_WORDS * sizeof(u32));
    s32 c = entry_count(wp);
    key[0] = (u32)wp->entries[0].count;
    for (s32 i = 0; i < c; i++) {
        const MapEntry* e = &wp->entries[i];
        key[1 + 3 * i] = kind == KIND_MAP ? (u32)(uintptr_t)e->objects : (u32)(uintptr_t)e->hitEntries;
        key[2 + 3 * i] = kind == KIND_MAP ? (u32)e->numJoints : (u32)e->hitNumJoints;
        key[3 + 3 * i] = (u32)(uintptr_t)e->dat;
    }
}

static int key_same(const Idx* x, const u32* key) { return memcmp(x->key, key, sizeof(x->key)) == 0; }

/* ---- builds ---- */

static void build_map(Idx* x, const MapWork* wp, const u32* key) {
    s32 c = entry_count(wp);
    u32 n = 0;
    for (s32 i = 0; i < c; i++) {
        n += wp->entries[i].numJoints > 0 ? (u32)wp->entries[i].numJoints : 0;
    }
    memcpy(x->key, key, sizeof(x->key));
    x->owner = wp;
    s_stats[KIND_MAP].rebuilds++;
    if (n > MAX_ITEMS || !idx_alloc(x, n)) {
        x->valid = -1;
        return;
    }
    u32 item = 0;
    x->n0 = c > 0 && wp->entries[0].numJoints > 0 ? (u32)wp->entries[0].numJoints : 0;
    for (s32 i = 0; i < c; i++) {
        const MapEntry* e = &wp->entries[i];
        const MapObject* o = e->objects;
        for (s32 j = 0; j < e->numJoints; j++, o++, item++) {
            if (o->joints != NULL && o->joints->name != NULL) {
                idx_insert(x, o->joints->name, item);
            }
        }
    }
    x->valid = 1;
}

static void build_hit(Idx* x, const MapWork* wp, const u32* key) {
    s32 c = entry_count(wp);
    u32 n = 0;
    for (s32 i = 0; i < c; i++) {
        n += wp->entries[i].hitNumJoints > 0 ? (u32)wp->entries[i].hitNumJoints : 0;
    }
    memcpy(x->key, key, sizeof(x->key));
    x->owner = wp;
    s_stats[KIND_HIT].rebuilds++;
    if (n > MAX_ITEMS || !idx_alloc(x, n)) {
        x->valid = -1;
        return;
    }
    u32 item = 0;
    x->n0 = c > 0 && wp->entries[0].hitNumJoints > 0 ? (u32)wp->entries[0].hitNumJoints : 0;
    for (s32 i = 0; i < c; i++) {
        const MapEntry* e = &wp->entries[i];
        const HitEntry* h = e->hitEntries;
        for (s32 j = 0; j < e->hitNumJoints; j++, h++, item++) {
            if (h->joint != NULL && h->joint->name != NULL) {
                idx_insert(x, h->joint->name, item);
            }
        }
    }
    x->valid = 1;
}

static void build_light(Idx* x, const LightWork* wp) {
    u32 n = wp->count > 0 ? (u32)wp->count : 0;
    memset(x->key, 0, sizeof(x->key));
    x->key[0] = (u32)(uintptr_t)wp->entries;
    x->key[1] = n;
    x->owner = wp;
    s_stats[KIND_LIGHT].rebuilds++;
    if (n > MAX_ITEMS || wp->entries == NULL || !idx_alloc(x, n)) {
        x->valid = -1;
        return;
    }
    for (u32 k = 0; k < n; k++) {
        const LightEntry* e = &wp->entries[k];
        char name[sizeof(e->name) + 1];
        memcpy(name, e->name, sizeof(e->name));
        name[sizeof(e->name)] = '\0'; /* strcmp stops at 32 bytes only if a NUL is there; the walk reads on */
        idx_insert(x, name, k);
    }
    x->valid = 1;
}

/* ---- walks (the game's loops, with probe counts) ---- */

static MapObject* walk_map(const MapWork* wp, const char* name, u32* probes) {
    for (s32 i = 0; i < wp->entries[0].count; i++) {
        const MapEntry* entry = &wp->entries[i];
        MapObject* object = entry->objects;
        for (s32 j = 0; j < entry->numJoints; j++, object++) {
            (*probes)++;
            if (!strcmp(object->joints->name, name)) {
                return object;
            }
        }
    }
    return NULL;
}

static HitEntry* walk_hit(const MapWork* wp, const char* name, u32* probes) {
    for (s32 i = 0; i < wp->entries->count; i++) {
        const MapEntry* map = &wp->entries[i];
        HitEntry* hit = map->hitEntries;
        for (s32 j = 0; j < map->hitNumJoints; j++, hit++) {
            (*probes)++;
            if (!(hit->flags & HIT_FLAG_DELETED) && !strcmp(hit->joint->name, name)) {
                return hit;
            }
        }
    }
    return NULL;
}

static LightEntry* walk_light(const LightWork* wp, const char* name, u32* probes) {
    LightEntry* entry = wp->entries;
    for (s32 i = 0; i < wp->count; i++, entry++) {
        (*probes)++;
        if (entry->flags & LIGHT_ENABLED && !strcmp(name, entry->name)) {
            return entry;
        }
    }
    return NULL;
}

/* ---- index lookups ---- */

static MapObject* find_map(const Idx* x, const MapWork* wp, const char* name, u32* probes, u32* compares) {
    u32 h = name_hash(name);
    u32 tag = h & 0xFFFF0000u;
    u32 s = home_of(h, x->mask);
    u32 v;
    while ((v = x->slots[s]) != 0) {
        (*probes)++;
        if ((v & 0xFFFF0000u) == tag) {
            u32 item = (v & 0xFFFFu) - 1;
            MapObject* o = item < x->n0 ? &wp->entries[0].objects[item] : &wp->entries[1].objects[item - x->n0];
            (*compares)++;
            if (!strcmp(o->joints->name, name)) {
                return o;
            }
        }
        s = (s + 1) & x->mask;
    }
    return NULL;
}

static HitEntry* find_hit(const Idx* x, const MapWork* wp, const char* name, u32* probes, u32* compares) {
    u32 h = name_hash(name);
    u32 tag = h & 0xFFFF0000u;
    u32 s = home_of(h, x->mask);
    u32 v;
    while ((v = x->slots[s]) != 0) {
        (*probes)++;
        if ((v & 0xFFFF0000u) == tag) {
            u32 item = (v & 0xFFFFu) - 1;
            HitEntry* e = item < x->n0 ? &wp->entries[0].hitEntries[item] : &wp->entries[1].hitEntries[item - x->n0];
            if (!(e->flags & HIT_FLAG_DELETED)) {
                (*compares)++;
                if (!strcmp(e->joint->name, name)) {
                    return e;
                }
            }
        }
        s = (s + 1) & x->mask;
    }
    return NULL;
}

static LightEntry* find_light(const Idx* x, const LightWork* wp, const char* name, u32* probes, u32* compares) {
    u32 h = name_hash(name);
    u32 tag = h & 0xFFFF0000u;
    u32 s = home_of(h, x->mask);
    u32 v;
    while ((v = x->slots[s]) != 0) {
        (*probes)++;
        if ((v & 0xFFFF0000u) == tag) {
            LightEntry* e = &wp->entries[(v & 0xFFFFu) - 1];
            if (e->flags & LIGHT_ENABLED) {
                (*compares)++;
                if (!strcmp(name, e->name)) {
                    return e;
                }
            }
        }
        s = (s + 1) & x->mask;
    }
    return NULL;
}

static void mismatch(int kind, const char* name, const void* idx, const void* walk) {
    s_stats[kind].mismatches++;
    if (s_logged_mismatch < 20) {
        s_logged_mismatch++;
        static const char* const k_kind[3] = {"map", "hit", "light"};
        PLOG_W("[NAMEIDX] mismatch (%s) \"%.32s\": index %p, walk %p", k_kind[kind], name, idx, walk);
    }
}

MapObject* portMapGetMapObj(MapWork* wp, const char* name, void* caller) {
    u32 probes = 0, compares = 0, p2 = 0; /* p2: the walk's probes in mode 2 (what gamecnt reports) */
    u64 t0 = port_gamecnt > 0 ? plat_ticks() : 0;
    MapObject* r;
    if (s_mode == 0) {
        r = walk_map(wp, name, &probes);
        compares = probes;
    } else {
        Idx* x = &s_idx[KIND_MAP][map_group(wp)];
        u32 key[KEY_WORDS];
        map_key(wp, KIND_MAP, key);
        if (x->valid == 0 || x->owner != wp || !key_same(x, key)) {
            build_map(x, wp, key);
        }
        if (x->valid < 0) {
            r = walk_map(wp, name, &probes);
            compares = probes;
        } else {
            r = find_map(x, wp, name, &probes, &compares);
            if (s_mode == 2) {
                MapObject* w = walk_map(wp, name, &p2);
                if (w != r) {
                    mismatch(KIND_MAP, name, r, w);
                    r = w;
                }
            }
        }
    }
    s_stats[KIND_MAP].lookups++;
    s_stats[KIND_MAP].probes += probes;
    s_stats[KIND_MAP].compares += compares;
    if (port_gamecnt_on()) {
        port_cnt_lookup_ticks(plat_ticks() - t0);
        port_cnt_lookup(KIND_MAP, caller, p2 ? p2 : probes, p2 ? p2 : compares);
    }
    return r;
}

HitEntry* portHitNameToPtr(MapWork* wp, const char* name, void* caller) {
    u32 probes = 0, compares = 0, p2 = 0; /* p2: the walk's probes in mode 2 (what gamecnt reports) */
    u64 t0 = port_gamecnt > 0 ? plat_ticks() : 0;
    HitEntry* r;
    if (s_mode == 0) {
        r = walk_hit(wp, name, &probes);
        compares = probes;
    } else {
        Idx* x = &s_idx[KIND_HIT][map_group(wp)];
        u32 key[KEY_WORDS];
        map_key(wp, KIND_HIT, key);
        if (x->valid == 0 || x->owner != wp || !key_same(x, key)) {
            build_hit(x, wp, key);
        }
        if (x->valid < 0) {
            r = walk_hit(wp, name, &probes);
            compares = probes;
        } else {
            r = find_hit(x, wp, name, &probes, &compares);
            if (s_mode == 2) {
                HitEntry* w = walk_hit(wp, name, &p2);
                if (w != r) {
                    mismatch(KIND_HIT, name, r, w);
                    r = w;
                }
            }
        }
    }
    s_stats[KIND_HIT].lookups++;
    s_stats[KIND_HIT].probes += probes;
    s_stats[KIND_HIT].compares += compares;
    if (port_gamecnt_on()) {
        port_cnt_lookup_ticks(plat_ticks() - t0);
        port_cnt_lookup(KIND_HIT, caller, p2 ? p2 : probes, p2 ? p2 : compares);
    }
    return r;
}

LightEntry* portLightNameToPtr(LightWork* wp, const char* name, void* caller) {
    u32 probes = 0, compares = 0, p2 = 0; /* p2: the walk's probes in mode 2 (what gamecnt reports) */
    u64 t0 = port_gamecnt > 0 ? plat_ticks() : 0;
    LightEntry* r;
    if (s_mode == 0) {
        r = walk_light(wp, name, &probes);
        compares = probes;
    } else {
        int g = s_idx[KIND_LIGHT][1].owner == wp ? 1 : s_idx[KIND_LIGHT][0].owner == wp ? 0 : s_idx[KIND_LIGHT][0].owner == NULL ? 0 : 1;
        Idx* x = &s_idx[KIND_LIGHT][g];
        if (x->valid == 0 || x->owner != wp || x->key[0] != (u32)(uintptr_t)wp->entries || x->key[1] != (u32)wp->count) {
            build_light(x, wp);
        }
        if (x->valid < 0) {
            r = walk_light(wp, name, &probes);
            compares = probes;
        } else {
            r = find_light(x, wp, name, &probes, &compares);
            if (s_mode == 2) {
                LightEntry* w = walk_light(wp, name, &p2);
                if (w != r) {
                    mismatch(KIND_LIGHT, name, r, w);
                    r = w;
                }
            }
        }
    }
    s_stats[KIND_LIGHT].lookups++;
    s_stats[KIND_LIGHT].probes += probes;
    s_stats[KIND_LIGHT].compares += compares;
    if (port_gamecnt_on()) {
        port_cnt_lookup_ticks(plat_ticks() - t0);
        port_cnt_lookup(KIND_LIGHT, caller, p2 ? p2 : probes, p2 ? p2 : compares);
    }
    return r;
}

/* [NAMEIDX] totals every 120 frames while lookups run (mapnameidx on). */
void port_nameidx_frame(u32 frame) {
    static u32 s_last_lookups;
    if (s_mode <= 0 || frame % 120 != 0) {
        return;
    }
    u32 all = s_stats[0].lookups + s_stats[1].lookups + s_stats[2].lookups;
    if (all == s_last_lookups) {
        return;
    }
    s_last_lookups = all;
    PLOG_I("[NAMEIDX] frame %lu: map %lu lookups %lu mismatches %lu rebuilds (%lu probes %lu compares) | hit %lu %lu %lu (%lu %lu) | "
           "light %lu %lu %lu (%lu %lu)",
           (unsigned long)frame, (unsigned long)s_stats[0].lookups, (unsigned long)s_stats[0].mismatches,
           (unsigned long)s_stats[0].rebuilds, (unsigned long)s_stats[0].probes, (unsigned long)s_stats[0].compares,
           (unsigned long)s_stats[1].lookups, (unsigned long)s_stats[1].mismatches, (unsigned long)s_stats[1].rebuilds,
           (unsigned long)s_stats[1].probes, (unsigned long)s_stats[1].compares, (unsigned long)s_stats[2].lookups,
           (unsigned long)s_stats[2].mismatches, (unsigned long)s_stats[2].rebuilds, (unsigned long)s_stats[2].probes,
           (unsigned long)s_stats[2].compares);
}
