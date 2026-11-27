/* Debug switches: words from the APT deliver arguments (devupdate restart),
 * sdmc:/3ds/ttyd/debug3ds.txt and the 3dslink arguments. Same rules as the Paper Mario 3DS port: a word matches whole
 * words only; key=value pairs are read with plat_switch_value. Lookups are
 * cached (a test run has a switch string; real play has none and returns at
 * once).
 *
 * Debug mode (docs/PORT_3DS_TESTING.md, "Debug mode") is decided here, at boot: the switch debug=0/1 (retail=1
 * is the old name for debug=0; a bare "debug" is debug=1, a bare "retail" is debug=0), else the Options setting
 * "debugmode" (on, off, or auto: on in a development build, off in a tagged release; the old Select-menu key
 * "retail=1" reads as off under auto); a TTYD_RETAIL build is always off. Debug mode off is "retail mode": every
 * switch reads as absent (the caller's default applies) except debug, retail and retailkeep, and the names listed
 * in retailkeep=a,b,c (a name ending in '*' is a prefix: retailkeep=map,story,gx*), the escape for tests that
 * measure Debug mode off. One rule in one place, no log line. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ctrdk/devupdate.h"
#include "plat/options.h"
#include "plat/plat.h"
#include "plat_internal.h"

#define SWITCH_TEXT 4096

static char s_text[SWITCH_TEXT];

typedef struct {
    char word[24];
    signed char value; /* 0 empty, 1 present, -1 absent */
} CacheEntry;
static CacheEntry s_cache[64];

int plat_retail_on = PLAT_RETAIL_BUILD; /* live, 1 = Debug mode off: the switch filter and the log read it */
static int s_retail_boot = PLAT_RETAIL_BUILD; /* the mode this launch booted in */
static int s_forced = -1;                     /* a debug=/retail= switch: the Debug mode it asks for, else -1 */
static char s_keep[256];                      /* retailkeep=..., commas and all */

static int scan(const char* word, const char** where) {
    size_t n = strlen(word);
    const char* p = s_text;
    while ((p = strstr(p, word)) != NULL) {
        int edge0 = p == s_text || (unsigned char)p[-1] <= ' ';
        char after = p[n];
        if (edge0 && ((unsigned char)after <= ' ' || after == '=')) {
            if (where != NULL) {
                *where = p;
            }
            return 1;
        }
        p += n;
    }
    return 0;
}

/* Retail mode: is this switch still read? */
static __attribute__((noinline, cold)) int retail_allows(const char* word) {
    static const char* const k_always[] = {"debug", "retail", "retailkeep", NULL};
    for (int i = 0; k_always[i] != NULL; i++) {
        if (strcmp(word, k_always[i]) == 0) {
            return 1;
        }
    }
    size_t wn = strlen(word);
    for (const char* p = s_keep; *p != '\0';) {
        const char* e = strchr(p, ',');
        size_t n = e != NULL ? (size_t)(e - p) : strlen(p);
        if (n > 0 && p[n - 1] == '*' ? (n - 1 <= wn && memcmp(p, word, n - 1) == 0)
                                     : (n == wn && memcmp(p, word, n) == 0)) {
            return 1;
        }
        p += n + (e != NULL);
    }
    return 0;
}

static int raw_value(const char* key, char* out, size_t n) {
    const char* p;
    if (!scan(key, &p)) {
        return 0;
    }
    p += strlen(key);
    if (*p != '=') {
        return 0;
    }
    p++;
    size_t i = 0;
    while (p[i] != '\0' && (unsigned char)p[i] > ' ' && i + 1 < n) {
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
    return 1;
}

int plat_switch_has(const char* word) {
    if (word == NULL || s_text[0] == '\0') {
        return 0;
    }
    if (__builtin_expect(plat_retail_on, 0) && !retail_allows(word)) {
        return 0;
    }
    size_t n = strlen(word);
    if (n >= sizeof(s_cache[0].word)) {
        return scan(word, NULL);
    }
    unsigned h = 5381;
    for (size_t i = 0; i < n; i++) {
        h = h * 33u + (unsigned char)word[i];
    }
    CacheEntry* e = &s_cache[h & 63u];
    if (e->value != 0 && strcmp(e->word, word) == 0) {
        return e->value > 0;
    }
    int r = scan(word, NULL);
    memcpy(e->word, word, n + 1);
    e->value = r ? 1 : -1;
    return r;
}

int plat_switch_value(const char* key, char* out, size_t n) {
    if (key == NULL || out == NULL || n == 0) {
        return 0;
    }
    if (__builtin_expect(plat_retail_on, 0) && !retail_allows(key)) {
        return 0;
    }
    return raw_value(key, out, n);
}

int plat_switch_int(const char* key, int fallback) {
    char v[24];
    if (!plat_switch_value(key, v, sizeof(v))) {
        return fallback;
    }
    char* end;
    long r = strtol(v, &end, 0);
    return end == v ? fallback : (int)r;
}

/* The stored setting (Options, Debug page): what a launch without a debug=/retail= switch does. */
static int setting_debug(void) {
    if (PLAT_RETAIL_BUILD) {
        return 0;
    }
    int mode = plat_opt_get(PLAT_OPT_KEY_DEBUGMODE, PLAT_OPT_DEBUG_AUTO);
    if (mode == PLAT_OPT_DEBUG_ON) {
        return 1;
    }
    if (mode == PLAT_OPT_DEBUG_OFF) {
        return 0;
    }
    if (plat_opt_get(PLAT_OPT_KEY_RETAIL, 0) != 0) {
        return 0; /* the old Select-menu "Retail mode: on" */
    }
    return plat_is_dev_build();
}

/* At boot, once the text is read: the switch first, then the stored setting. */
static void retail_decide(void) {
    s_forced = -1;
    if (!PLAT_RETAIL_BUILD) {
        char v[8];
        if (raw_value("debug", v, sizeof(v))) {
            s_forced = atoi(v) != 0;
        } else if (scan("debug", NULL)) {
            s_forced = 1;
        } else if (raw_value("retail", v, sizeof(v))) {
            s_forced = atoi(v) == 0;
        } else if (scan("retail", NULL)) {
            s_forced = 0;
        }
    }
    int debug = s_forced >= 0 ? s_forced : setting_debug();
    s_keep[0] = '\0';
    raw_value("retailkeep", s_keep, sizeof(s_keep));
    s_retail_boot = !debug;
    plat_retail_on = !debug;
}

int plat_retail_booted(void) { return s_retail_boot; }
int plat_retail_keeps(const char* name) { return retail_allows(name); }
int plat_debug_forced(void) { return !PLAT_RETAIL_BUILD && s_forced >= 0; }
int plat_debug_wanted(void) { return PLAT_RETAIL_BUILD ? 0 : s_forced >= 0 ? s_forced : setting_debug(); }

/* What changes at once when the setting changes (either menu). Off: logging stops (nothing is formatted or
 * queued), every later switch lookup is filtered, the HUD drops to fps and the memory log stops; the threads that
 * already run (log writer, update server, supervisor heartbeat, watchdog, guard) and a stats switch's counters
 * (callers cache their switch at first use) stay until the next launch. On: the same at once when this launch
 * booted with Debug mode on, else at the next launch (no log file, queue or writer exists yet). */
void plat_debug_option_changed(void) {
    if (PLAT_RETAIL_BUILD) {
        return;
    }
    if (plat_opt_get(PLAT_OPT_KEY_RETAIL, 0) != 0) {
        plat_opt_set(PLAT_OPT_KEY_RETAIL, 0); /* the old key: the new setting replaces it */
    }
    if (!plat_debug_wanted()) {
        plat_retail_on = 1;
        plat_log_retail_now(1);
    } else if (!s_retail_boot) {
        plat_retail_on = 0;
        plat_log_retail_now(0);
    }
}

/* The Select menu's Debug tab row: stores on or off (not auto); plat_opt_set calls plat_debug_option_changed. */
void plat_debug_set(int on) {
    if (PLAT_RETAIL_BUILD) {
        return;
    }
    plat_opt_set(PLAT_OPT_KEY_DEBUGMODE, on ? PLAT_OPT_DEBUG_ON : PLAT_OPT_DEBUG_OFF);
    plat_options_save();
}

void plat_switches_load(int argc, char** argv) {
    s_text[0] = '\0';
    /* First the switches this launch received as APT deliver arguments (a
     * devupdate restart): a lookup takes the first match, so these one-shot
     * switches win over the file and the 3dslink arguments. */
    snprintf(s_text, sizeof(s_text), "%s", devupdate_launch_args());
    FILE* f = fopen("debug3ds.txt", "r");
    if (f != NULL) {
        size_t len = strlen(s_text);
        if (len > 0 && len + 1 < sizeof(s_text)) {
            s_text[len++] = ' ';
        }
        size_t got = fread(s_text + len, 1, sizeof(s_text) - 1 - len, f);
        fclose(f);
        s_text[len + got] = '\0';
    }
    /* 3dslink arguments after the program name. */
    for (int i = 1; i < argc; i++) {
        size_t len = strlen(s_text);
        if (len + strlen(argv[i]) + 2 >= sizeof(s_text)) {
            break;
        }
        snprintf(s_text + len, sizeof(s_text) - len, "%s%s", len ? " " : "", argv[i]);
    }
    for (char* c = s_text; *c != '\0'; c++) {
        if (*c == '\r' || *c == '\n' || *c == '\t') {
            *c = ' ';
        }
    }
    memset(s_cache, 0, sizeof(s_cache));
    retail_decide();
}

const char* plat_switches_text(void) {
    return s_text;
}
