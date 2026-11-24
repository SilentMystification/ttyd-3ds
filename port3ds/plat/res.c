/* The port's own resource files (include/plat/res.h). */
#include <stdio.h>
#include <string.h>

#include "plat/res.h"
#include "plat/sdio.h"

#define RES_MAX 4
#define NAME_MAX_LEN 48

static const char* const kDirs[] = {"res", "queue/res"};

static struct {
    char name[NAME_MAX_LEN];
    void* data;
    size_t n;
} s_cache[RES_MAX];

const void* plat_res_get(const char* name, size_t* n) {
    if (name == NULL || strlen(name) >= NAME_MAX_LEN) {
        return NULL;
    }
    for (int i = 0; i < RES_MAX; i++) {
        if (s_cache[i].data != NULL && strcmp(s_cache[i].name, name) == 0) {
            if (n != NULL) {
                *n = s_cache[i].n;
            }
            return s_cache[i].data;
        }
    }
    for (size_t d = 0; d < sizeof(kDirs) / sizeof(kDirs[0]); d++) {
        char path[96];
        size_t got = 0;
        snprintf(path, sizeof(path), "%s/%s", kDirs[d], name);
        void* data = plat_sd_read_file(path, &got);
        if (data == NULL || got == 0) {
            continue;
        }
        for (int i = 0; i < RES_MAX; i++) {
            if (s_cache[i].data == NULL) {
                strcpy(s_cache[i].name, name);
                s_cache[i].data = data;
                s_cache[i].n = got;
                break;
            }
        }
        if (n != NULL) {
            *n = got;
        }
        return data;
    }
    return NULL;
}

const char* plat_res_path(const char* name) {
    static char s_path[96];
    snprintf(s_path, sizeof(s_path), "sdmc:/3ds/ttyd/res/%s", name);
    return s_path;
}
