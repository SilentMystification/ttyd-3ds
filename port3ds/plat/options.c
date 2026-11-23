/* Player options store (see include/plat/options.h).
 *
 * sdmc:/3ds/ttyd/options.txt (the working directory is sdmc:/3ds/ttyd):
 *   # ttyd3ds options v1
 *   volume=100
 *   ...
 *   #end
 * The "#end" line says the write finished. A file without it (cut off) or with
 * lines that are not "key=value" is ignored as a whole, with a log line, and
 * the defaults apply. The file is written to options.tmp and renamed over
 * options.txt; the FAT file system cannot rename onto an existing file, so the
 * old one is removed first, and a leftover options.tmp that is complete is read
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
#define OPT_TMP "options.tmp"
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
static int read_file(const char* path) {
    FILE* f = fopen(path, "r");
    if (f == NULL) {
        return 0;
    }
    Entry tmp[MAX_ENTRIES];
    int n = 0, ended = 0, bad = 0;
    char line[96];
    while (fgets(line, sizeof(line), f) != NULL) {
        size_t len = strlen(line);
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
    fclose(f);
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
        if (!read_file(OPT_FILE)) {
            FILE* f = fopen(OPT_FILE, "r");
            if (f != NULL) {
                fclose(f); /* options.txt exists but was bad: defaults */
            } else if (!read_file(OPT_TMP)) {
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
    FILE* f = fopen(OPT_TMP, "w");
    if (f == NULL) {
        PLOG_E("options: cannot write %s (%d)", OPT_TMP, errno);
        unlock();
        return;
    }
    fprintf(f, "# ttyd3ds options v1\n");
    int ok = 1;
    if (s_menu != NULL) {
        for (int i = 0; i < s_menu->item_count; i++) {
            const PlatOptItem* it = &s_menu->items[i];
            if (it->key == NULL) {
                continue;
            }
            if (it->kind == PLAT_OPT_CHOICE) {
                fprintf(f, "# %s:", it->key);
                for (int k = 0; k < name_count(it); k++) {
                    fprintf(f, " %d=%s", k, it->names[k]);
                }
                fprintf(f, "\n");
            } else {
                fprintf(f, "# %s: %d..%d\n", it->key, (int)it->min, (int)it->max);
            }
        }
    }
    for (int i = 0; i < s_n; i++) {
        fprintf(f, "%s=%d\n", s_ent[i].key, s_ent[i].value);
    }
    fprintf(f, "#end\n");
    if (fflush(f) != 0 || ferror(f)) {
        ok = 0;
    }
    if (fclose(f) != 0) {
        ok = 0;
    }
    if (!ok) {
        PLOG_E("options: write to %s failed", OPT_TMP);
        remove(OPT_TMP);
    } else {
        remove(OPT_FILE);
        if (rename(OPT_TMP, OPT_FILE) != 0) {
            PLOG_E("options: rename %s -> %s failed (%d)", OPT_TMP, OPT_FILE, errno);
        } else {
            s_dirty = 0;
            PLOG_I("options: saved %d values to %s", s_n, OPT_FILE);
        }
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
int opt_get_texfilter(void) {
    int v = plat_opt_get(PLAT_OPT_KEY_TEXFILTER, PLAT_OPT_TEXFILTER_GAME);
    return v < PLAT_OPT_TEXFILTER_GAME || v > PLAT_OPT_TEXFILTER_NEAREST ? PLAT_OPT_TEXFILTER_GAME : v;
}
int opt_get_language(void) {
    int v = plat_opt_get(PLAT_OPT_KEY_LANGUAGE, 0);
    return v < 0 ? 0 : v;
}
