/* Player options store (see include/plat/options.h).
 *
 * sdmc:/3ds/ttyd/options.txt (the working directory is sdmc:/3ds/ttyd):
 *   # ttyd3ds options v1
 *   volume=100
 *   ...
 *   #end
 * The "#end" line says the write finished. A file without it (cut off) or with
 * lines that are not "key=value" is ignored as a whole, with a log line, and
 * the defaults apply. The file is written (by the SD service, plat/sdio.c) to options.txt.tmp and renamed over
 * options.txt; the FAT file system cannot rename onto an existing file, so the
 * old one is removed first, and a leftover options.txt.tmp that is complete is read
 * when options.txt is missing (a crash between the two steps loses nothing).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plat/audio.h"
#include "plat/options.h"
#include "plat_internal.h"

#define OPT_FILE "options.txt"
#define OPT_TMP "options.txt.tmp"
#define MAX_ENTRIES 64
#define KEY_LEN 24

typedef struct {
    char key[KEY_LEN];
    int value;
} Entry;

static Entry s_ent[MAX_ENTRIES];
static int s_n;
static volatile int s_loaded;
static volatile int s_dirty;
static LightLock s_lock;
static int s_lock_init;
static const PlatOptMenu* s_menu;
static int s_applied_volume = -1;
static int s_applied_output;

static void lock(void) {
    if (!__atomic_load_n(&s_lock_init, __ATOMIC_ACQUIRE)) {
        /* first use happens on one thread in practice; the init is idempotent */
        LightLock_Init(&s_lock);
        __atomic_store_n(&s_lock_init, 1, __ATOMIC_RELEASE);
    }
    LightLock_Lock(&s_lock);
}

static void unlock(void) { LightLock_Unlock(&s_lock); }

static int name_count(const PlatOptItem* it) {
    int n = 0;
    while (it->names != NULL && it->names[n] != NULL) {
        n++;
    }
    return n;
}

static const PlatOptItem* find_def(const char* key) {
    if (s_menu == NULL) {
        return NULL;
    }
    for (int i = 0; i < s_menu->item_count; i++) {
        const PlatOptItem* it = &s_menu->items[i];
        if (it->key != NULL && strcmp(it->key, key) == 0) {
            return it;
        }
    }
    return NULL;
}

static int valid(const PlatOptItem* it, int v) {
    if (it->kind == PLAT_OPT_CHOICE) {
        return v >= 0 && v < name_count(it);
    }
    return v >= it->min && v <= it->max;
}

static Entry* find_entry(const char* key) {
    for (int i = 0; i < s_n; i++) {
        if (strcmp(s_ent[i].key, key) == 0) {
            return &s_ent[i];
        }
    }
    return NULL;
}

static void put_entry(const char* key, int value) {
    Entry* e = find_entry(key);
    if (e == NULL) {
        if (s_n >= MAX_ENTRIES) {
            PLOG_W("options: too many keys, \"%s\" ignored", key);
            return;
        }
        e = &s_ent[s_n];
        snprintf(e->key, sizeof(e->key), "%s", key);
        e->value = value;
        __atomic_store_n(&s_n, s_n + 1, __ATOMIC_RELEASE);
        return;
    }
    e->value = value;
}

/* Reads one file into the table. Returns 1 when it was complete and well formed. */
static int read_file(const char* path, int* exists) {
    size_t text_n = 0;
    char* text = plat_sd_read_file(path, &text_n);
    if (exists != NULL) {
        *exists = text != NULL;
    }
    if (text == NULL) {
        return 0;
    }
    Entry tmp[MAX_ENTRIES];
    int n = 0, ended = 0, bad = 0;
    char line[96];
    for (char* p = text; *p != '\0';) {
        char* nl = strchr(p, '\n');
        size_t len = nl != NULL ? (size_t)(nl - p) : strlen(p);
        if (len >= sizeof(line)) {
            len = sizeof(line) - 1;
        }
        memcpy(line, p, len);
        line[len] = '\0';
        p = nl != NULL ? nl + 1 : p + strlen(p);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' ')) {
            line[--len] = '\0';
        }
        if (len == 0) {
            continue;
        }
        if (line[0] == '#') {
            if (strcmp(line, "#end") == 0) {
                ended = 1;
            }
            continue;
        }
        char* eq = strchr(line, '=');
        if (eq == NULL || eq == line || (size_t)(eq - line) >= KEY_LEN || eq[1] == '\0') {
            bad++;
            continue;
        }
        *eq = '\0';
        char* end;
        long v = strtol(eq + 1, &end, 10);
        if (*end != '\0' || n >= MAX_ENTRIES) {
            bad++;
            continue;
        }
        snprintf(tmp[n].key, sizeof(tmp[n].key), "%s", line);
        tmp[n].value = (int)v;
        n++;
    }
    free(text);
    if (!ended || bad) {
        PLOG_W("options: %s is %s; using the defaults", path, !ended ? "incomplete or corrupt" : "corrupt");
        return 0;
    }
    for (int i = 0; i < n; i++) {
        put_entry(tmp[i].key, tmp[i].value);
    }
    PLOG_I("options: read %d values from %s", n, path);
    return 1;
}

static void ensure_loaded(void) {
    if (__atomic_load_n(&s_loaded, __ATOMIC_ACQUIRE)) {
        return;
    }
    lock();
    if (!s_loaded) {
        int exists = 0;
        if (!read_file(OPT_FILE, &exists)) {
            if (exists) {
                /* options.txt exists but was bad: defaults */
            } else if (!read_file(OPT_TMP, NULL)) {
                PLOG_I("options: no options file, using the defaults");
            }
        }
        __atomic_store_n(&s_loaded, 1, __ATOMIC_RELEASE);
    }
    unlock();
}

int plat_opt_get(const char* key, int fallback) {
    ensure_loaded();
    const PlatOptItem* def = find_def(key);
    int dv = def != NULL ? def->def : fallback;
    int n = __atomic_load_n(&s_n, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) {
        if (strcmp(s_ent[i].key, key) == 0) {
            int v = s_ent[i].value;
            if (def != NULL && !valid(def, v)) {
                return dv;
            }
            return v;
        }
    }
    return dv;
}

void plat_opt_set(const char* key, int value) {
    ensure_loaded();
    const PlatOptItem* def = find_def(key);
    if (def != NULL && !valid(def, value)) {
        value = def->def;
    }
    lock();
    int changed = find_entry(key) == NULL || find_entry(key)->value != value;
    put_entry(key, value);
    if (changed) {
        s_dirty = 1;
    }
    unlock();
    if (changed && s_menu != NULL && s_menu->on_change != NULL) {
        s_menu->on_change(key, value);
    }
}

void plat_options_register(const PlatOptMenu* menu) {
    ensure_loaded();
    s_menu = menu;
    for (int i = 0; i < menu->item_count; i++) {
        const PlatOptItem* it = &menu->items[i];
        if (it->key != NULL && menu->on_change != NULL) {
            menu->on_change(it->key, plat_opt_get(it->key, it->def));
        }
    }
}

const PlatOptMenu* plat_options_menu(void) { return s_menu; }

void plat_options_save(void) {
    if (!s_dirty) {
        return;
    }
    lock();
    /* The text is built here and handed to the SD service (core 1) as an atomic write: the caller
     * never waits on the card. A failure is logged by the worker. */
    size_t cap = 4096, len = 0;
    char* buf = malloc(cap);
    if (buf == NULL) {
        PLOG_E("options: no memory to write %s", OPT_FILE);
        unlock();
        return;
    }
#define APP(...) \
    do { \
        if (len + 256 > cap) { \
            cap *= 2; \
            char* nb = realloc(buf, cap); \
            if (nb == NULL) { \
                free(buf); \
                PLOG_E("options: no memory to write %s", OPT_FILE); \
                unlock(); \
                return; \
            } \
            buf = nb; \
        } \
        len += (size_t)snprintf(buf + len, cap - len, __VA_ARGS__); \
    } while (0)
    APP("# ttyd3ds options v1\n");
    if (s_menu != NULL) {
        for (int i = 0; i < s_menu->item_count; i++) {
            const PlatOptItem* it = &s_menu->items[i];
            if (it->key == NULL) {
                continue;
            }
            if (it->kind == PLAT_OPT_CHOICE) {
                APP("# %s:", it->key);
                for (int k = 0; k < name_count(it); k++) {
                    APP(" %d=%s", k, it->names[k]);
                }
                APP("\n");
            } else {
                APP("# %s: %d..%d\n", it->key, (int)it->min, (int)it->max);
            }
        }
    }
    for (int i = 0; i < s_n; i++) {
        APP("%s=%d\n", s_ent[i].key, s_ent[i].value);
    }
    APP("#end\n");
#undef APP
    if (plat_sd_write_file_own(OPT_FILE, buf, len, PLAT_SD_ATOMIC) == 0) {
        s_dirty = 0;
        PLOG_I("options: saving %d values to %s", s_n, OPT_FILE);
    } else {
        PLOG_E("options: cannot queue the write of %s", OPT_FILE);
    }
    unlock();
}

/* Once per frame (plat/input.c): applies what the platform itself owns. */
void plat_options_tick(void) {
    int out = plat_audio_has_output();
    int vol = opt_get_volume();
    if (out && (vol != s_applied_volume || !s_applied_output)) {
        ndspSetMasterVol((float)vol / 100.0f);
        s_applied_volume = vol;
        s_applied_output = 1;
        PLOG_I("options: master volume %d%%", vol);
    }
}

int opt_get_stereo3d(void) { return plat_opt_get(PLAT_OPT_KEY_STEREO3D, 1) != 0; }
int opt_get_hud_mode(void) {
    int v = plat_opt_get(PLAT_OPT_KEY_HUD, PLAT_OPT_HUD_FULL);
    return v < PLAT_OPT_HUD_OFF || v > PLAT_OPT_HUD_FULL ? PLAT_OPT_HUD_FULL : v;
}
int opt_get_volume(void) {
    int v = plat_opt_get(PLAT_OPT_KEY_VOLUME, 100);
    return v < 0 ? 0 : v > 100 ? 100 : v;
}
int opt_get_quality(void) {
    int v = plat_opt_get(PLAT_OPT_KEY_QUALITY, PLAT_OPT_QUALITY_AUTO);
    return v < PLAT_OPT_QUALITY_AUTO || v > PLAT_OPT_QUALITY_FAST ? PLAT_OPT_QUALITY_AUTO : v;
}
int opt_get_language(void) {
    int v = plat_opt_get(PLAT_OPT_KEY_LANGUAGE, 0);
    return v < 0 ? 0 : v;
}
