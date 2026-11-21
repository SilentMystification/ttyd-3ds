/* Debug switches: words from sdmc:/3ds/ttyd/debug3ds.txt and the 3dslink
 * arguments. Same rules as the Paper Mario 3DS port: a word matches whole
 * words only; key=value pairs are read with plat_switch_value. Lookups are
 * cached (a test run has a switch string; real play has none and returns at
 * once). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plat/plat.h"
#include "plat_internal.h"

#define SWITCH_TEXT 512

static char s_text[SWITCH_TEXT];

typedef struct {
    char word[24];
    signed char value; /* 0 empty, 1 present, -1 absent */
} CacheEntry;
static CacheEntry s_cache[64];

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

int plat_switch_has(const char* word) {
    if (word == NULL || s_text[0] == '\0') {
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
    const char* p;
    if (key == NULL || out == NULL || n == 0 || !scan(key, &p)) {
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

int plat_switch_int(const char* key, int fallback) {
    char v[24];
    if (!plat_switch_value(key, v, sizeof(v))) {
        return fallback;
    }
    char* end;
    long r = strtol(v, &end, 0);
    return end == v ? fallback : (int)r;
}

void plat_switches_load(int argc, char** argv) {
    s_text[0] = '\0';
    FILE* f = fopen("debug3ds.txt", "r");
    if (f != NULL) {
        size_t got = fread(s_text, 1, sizeof(s_text) - 1, f);
        fclose(f);
        s_text[got] = '\0';
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
}

const char* plat_switches_text(void) {
    return s_text;
}
