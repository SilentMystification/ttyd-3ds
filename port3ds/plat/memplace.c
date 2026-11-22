/* Memory placement: named buffer regions packed into memory tiers.
 *
 * Tiers: QTM (the New 3DS extra RAM), LINEAR (linear heap) and HEAP (cached
 * FCRAM). A placement is registered with plat_place_register (name, size,
 * alignment, ordered tier preferences, who-touches-it flags); the platform
 * packs the registrations by priority at boot. API and rules: plat.h.
 *
 * The CIA's ExHeader maps the extra RAM ("QTM RAM") at 0x1E800000
 * (port3ds/rsf/ttyd.rsf). The application part is the first 2 MiB; the
 * rest (0x1EA00000 on) belongs to the system qtm service and the kernel,
 * and writing there crashed the qtm sysmodule. It holds state
 * that several cores touch every frame, keeping that traffic off the FCRAM
 * bus the GPU and DSP use. Whether the GPU can reach it is unverified on
 * hardware (Azahar's GPU cannot, see docs/PORT_3DS_TESTING.md), so a placement
 * the GPU touches only goes to QTM with the switch qtmgpu; the DSP never
 * does. When the mapping is absent (a 3dsx) QTM is not a candidate, the next
 * tier of each placement is used: behaviour is the same, only speed differs,
 * and the fallback is reported in the log and on the HUD.
 *
 * The platform's own four placements are registered in plat_memplace_init
 * (they keep their PLAT_PLACE_* ids as handles, for log.c and gpu.c). A game
 * registers its own from a manifest function given to
 * plat_place_set_manifest, which runs before packing.
 */
#include <ctype.h>
#include <malloc.h>
#include <string.h>

#include "plat_internal.h"

#define QTM_VADDR 0x1E800000u
#define QTM_BYTES 0x00200000u /* application part only */
#define MAX_PLACE 32
#define MAX_FORCE 16
#define MAX_PREFS 4

typedef struct {
    const char* name;
    u32 budget;
    u32 align;
    u32 used;
    u32 flags;
    int prio;
    u8 prefs[MAX_PREFS + 1];
    int tier; /* PLAT_TIER_*, -1 not placed yet */
    int forced;
    u8* base;
} Placement;

typedef struct {
    char name[40];
    int tier;
} Force;

static Placement s_table[MAX_PLACE];
static int s_count;
static Force s_force[MAX_FORCE];
static int s_force_count;
static void (*s_manifest)(void);
static int s_qtm;
static int s_qtmgpu;
static int s_packed;
static u32 s_qtm_next; /* offset of the next free QTM byte */

static const char* const k_tier_name[PLAT_TIER_COUNT] = {"QTM", "LINEAR", "HEAP"};

int plat_qtm_available(void) { return s_qtm; }

static int qtm_mapped(void) {
    MemInfo mi;
    PageInfo pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, QTM_VADDR))) {
        return 0;
    }
    return mi.base_addr <= QTM_VADDR && mi.base_addr + mi.size >= QTM_VADDR + QTM_BYTES &&
           (mi.perm & (MEMPERM_READ | MEMPERM_WRITE)) == (MEMPERM_READ | MEMPERM_WRITE) && mi.state != MEMSTATE_FREE;
}

/* Names match ignoring case, and '_' stands for ' ' (switch values end at a space). */
static int name_eq(const char* a, const char* b) {
    for (;; a++, b++) {
        int x = *a == '_' ? ' ' : tolower((unsigned char)*a);
        int y = *b == '_' ? ' ' : tolower((unsigned char)*b);
        if (x != y) {
            return 0;
        }
        if (x == 0) {
            return 1;
        }
    }
}

static void plat_memplace_place(int h);

void plat_place_set_manifest(void (*fn)(void)) { s_manifest = fn; }

int plat_place_register(const char* name, size_t bytes, size_t align, const uint8_t* prefs, unsigned flags) {
    if (name == NULL || prefs == NULL || bytes == 0 || bytes > 0x10000000u || align == 0 || (align & (align - 1)) != 0 ||
        s_count >= MAX_PLACE) {
        PLOG_E("[MEM] register \"%s\" refused (%lu bytes, align %lu, %d registered)", name ? name : "(null)",
               (unsigned long)bytes, (unsigned long)align, s_count);
        return -1;
    }
    Placement* p = &s_table[s_count];
    memset(p, 0, sizeof(*p));
    p->name = name;
    p->budget = (u32)bytes;
    p->align = align < 64 ? 64u : (u32)align;
    p->flags = flags;
    p->prio = s_count; /* registration order unless plat_place_priority says otherwise */
    p->tier = -1;
    p->forced = -1;
    int n = 0;
    for (int i = 0; prefs[i] != PLAT_TIER_NONE; i++) {
        if (prefs[i] >= PLAT_TIER_COUNT || n >= MAX_PREFS) {
            PLOG_E("[MEM] register \"%s\": bad tier list", name);
            return -1;
        }
        p->prefs[n++] = prefs[i];
    }
    p->prefs[n] = PLAT_TIER_NONE;
    if (n == 0) {
        PLOG_E("[MEM] register \"%s\": empty tier list", name);
        return -1;
    }
    int h = s_count++;
    if (s_packed) {
        plat_memplace_place(h);
    }
    return h;
}

void plat_place_priority(int handle, int priority) {
    if (handle >= 0 && handle < s_count) {
        s_table[handle].prio = priority;
    }
}

static int tier_allowed(const Placement* p, int tier) {
    if (tier == PLAT_TIER_QTM) {
        if (!s_qtm || (p->flags & PLAT_PF_DSP)) {
            return 0;
        }
        if ((p->flags & (PLAT_PF_GPU_READ | PLAT_PF_GPU_WRITE)) && !s_qtmgpu) {
            return 0;
        }
    }
    return 1;
}

static int tier_take(Placement* p, int tier) {
    switch (tier) {
    case PLAT_TIER_QTM:
        if (s_qtm_next + p->budget > QTM_BYTES) {
            return 0;
        }
        p->base = (u8*)QTM_VADDR + s_qtm_next;
        s_qtm_next += (p->budget + 63u) & ~63u;
        return 1;
    case PLAT_TIER_LINEAR:
        p->base = linearMemAlign(p->budget, p->align);
        return p->base != NULL;
    case PLAT_TIER_HEAP:
        p->base = memalign(p->align, p->budget);
        return p->base != NULL;
    }
    return 0;
}

/* Choose a tier for one registration: the forced tier first, then its list. */
static void plat_memplace_place(int h) {
    Placement* p = &s_table[h];
    int tries[MAX_PREFS + 1];
    int n = 0;
    for (int i = 0; i < s_force_count; i++) {
        if (name_eq(s_force[i].name, p->name)) {
            p->forced = s_force[i].tier;
            tries[n++] = p->forced;
        }
    }
    for (int i = 0; p->prefs[i] != PLAT_TIER_NONE; i++) {
        tries[n++] = p->prefs[i];
    }
    for (int i = 0; i < n; i++) {
        int t = tries[i];
        if (!tier_allowed(p, t)) {
            if (i == 0 && p->forced >= 0) {
                PLOG_W("[MEM] %s: forced tier %s refused by the placement rules", p->name, k_tier_name[t]);
            }
            continue;
        }
        if (tier_take(p, t)) {
            p->tier = t;
            p->used = 0;
            PLOG_I("[MEM] placed %-11s %7lu bytes in %s%s", p->name, (unsigned long)p->budget, k_tier_name[t],
                   (i == 0 && p->forced == t) ? " (forced)" : "");
            return;
        }
        PLOG_W("[MEM] %s: %s has no room for %lu bytes", p->name, k_tier_name[t], (unsigned long)p->budget);
    }
    plat_fatal("memplace: no tier for %s (%lu bytes)", p->name, (unsigned long)p->budget);
}

/* "place=<name>:<tier>[,<name>:<tier>...]": store the valid pairs, log the rest. Names
 * are checked against what is registered when packing starts; a name registered
 * later still works, it is only logged as unknown here. */
static void parse_place_switch(void) {
    char val[256];
    if (!plat_switch_value("place", val, sizeof(val))) {
        return;
    }
    char* save = val;
    while (*save != '\0') {
        char* item = save;
        char* comma = strchr(item, ',');
        if (comma != NULL) {
            *comma = '\0';
            save = comma + 1;
        } else {
            save += strlen(item);
        }
        char* colon = strrchr(item, ':');
        if (colon == NULL || colon == item) {
            PLOG_W("[MEM] place=%s ignored: want <name>:<tier>", item);
            continue;
        }
        *colon = '\0';
        const char* tname = colon + 1;
        int tier = -1;
        for (int t = 0; t < PLAT_TIER_COUNT; t++) {
            if (name_eq(tname, k_tier_name[t])) {
                tier = t;
            }
        }
        int known = 0;
        for (int i = 0; i < s_count; i++) {
            known |= name_eq(item, s_table[i].name);
        }
        if (tier < 0) {
            PLOG_W("[MEM] place=%s:%s ignored: unknown tier (qtm, linear, heap)", item, tname);
        } else if (!known) {
            PLOG_W("[MEM] place=%s:%s ignored: no such placement", item, tname);
        } else if (s_force_count >= MAX_FORCE || strlen(item) >= sizeof(s_force[0].name)) {
            PLOG_W("[MEM] place=%s:%s ignored: too many or too long", item, tname);
        } else {
            strcpy(s_force[s_force_count].name, item);
            s_force[s_force_count].tier = tier;
            s_force_count++;
        }
    }
}

void plat_memplace_init(void) {
    /* The platform's own placements; their handles are the PLAT_PLACE_* ids. */
    static const struct {
        const char* name;
        u32 bytes;
        unsigned flags;
    } k_own[PLAT_PLACE_COUNT] = {
        [PLAT_PLACE_GX_RECORD] = {"gx record", 1u << 20, PLAT_PF_CPU_SHARED},
        [PLAT_PLACE_GX_STATE] = {"gx state", 128u << 10, PLAT_PF_CPU_SHARED},
        [PLAT_PLACE_AUDIO_STATE] = {"audio state", 128u << 10, PLAT_PF_CPU_SHARED},
        [PLAT_PLACE_LOG] = {"log", 320u << 10, PLAT_PF_CPU_SHARED},
    };
    for (int i = 0; i < PLAT_PLACE_COUNT; i++) {
        plat_place_register(k_own[i].name, k_own[i].bytes, 64, PLAT_PREFS(PLAT_TIER_QTM, PLAT_TIER_HEAP), k_own[i].flags);
    }
    if (s_manifest != NULL) {
        s_manifest();
    }
    /* The first 2 MiB of QTM RAM is the application part; noqtm puts the
     * buffers back in FCRAM (for comparison or a bad QTM). */
    int mapped = qtm_mapped();
    s_qtm = !plat_switch_has("noqtm") && mapped;
    s_qtmgpu = plat_switch_has("qtmgpu");
    parse_place_switch();

    /* Pack by priority (registration order among equals) into the tiers. */
    int order[MAX_PLACE];
    for (int i = 0; i < s_count; i++) {
        int j = i;
        while (j > 0 && s_table[order[j - 1]].prio > s_table[i].prio) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = i;
    }
    for (int i = 0; i < s_count; i++) {
        plat_memplace_place(order[i]);
    }
    s_packed = 1;

    if (s_qtm) {
        PLOG_I("[MEM] QTM mapped at %08lx, %lu of %lu bytes placed", (unsigned long)QTM_VADDR,
               (unsigned long)s_qtm_next, (unsigned long)QTM_BYTES);
    } else if (mapped) {
        PLOG_W("[MEM] QTM mapped but switched off (noqtm), using FCRAM");
    } else {
        PLOG_W("[MEM] QTM unavailable, using FCRAM");
    }
}

void* plat_place_alloc(int id, size_t size, size_t align) {
    if (id < 0 || id >= s_count || align == 0 || (align & (align - 1)) != 0) {
        return NULL;
    }
    Placement* p = &s_table[id];
    if (p->base == NULL) {
        return NULL;
    }
    u32 start = (p->used + (u32)align - 1u) & ~((u32)align - 1u);
    if (start > p->budget || size > p->budget - start) {
        PLOG_E("[MEM] %s: %lu bytes do not fit (%lu of %lu used)", p->name, (unsigned long)size,
               (unsigned long)p->used, (unsigned long)p->budget);
        return NULL;
    }
    p->used = start + (u32)size;
    return p->base + start;
}

void plat_memplace_report(void) {
    for (int i = 0; i < s_count; i++) {
        PLOG_I("[MEM] %-11s %7lu of %7lu bytes (%s)", s_table[i].name, (unsigned long)s_table[i].used,
               (unsigned long)s_table[i].budget, s_table[i].tier >= 0 ? k_tier_name[s_table[i].tier] : "unplaced");
    }
}
