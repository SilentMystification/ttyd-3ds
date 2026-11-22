/* TTYD's converter table for the host decode tool: which disc files the port converts, with which of the port's
 * converters, and how the game reads them (src/manager/filemgr.c, mapdrv.c, cam_road.c, fontmgr.c, sound.c,
 * icondrv.c). The path rules are paper-cut's (tools/port/disc_sweep.py) plus the port's own sound and icon files.
 *
 * Another game's tool replaces this file; main.c, fst.c and shim.c do not change. */
#include <string.h>

#include "host_decode.h"
#include "port_formats.h"
#include "port_musyx_swap.h"

static int ends_with(const char* s, const char* suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

static int starts_with(const char* s, const char* prefix) { return strncmp(s, prefix, strlen(prefix)) == 0; }

/* "m/<map>/<leaf>" */
static int map_file(const char* p, const char* leaf) {
    if (!starts_with(p, "m/")) {
        return 0;
    }
    const char* slash = strchr(p + 2, '/');
    return slash != NULL && strchr(slash + 1, '/') == NULL && strcmp(slash + 1, leaf) == 0;
}

/* "a/<name>" with no extension and no '-' suffix: an animated model; "a/<name>-" is its texture file */
static int anim_dir_file(const char* p) { return starts_with(p, "a/") && strchr(p + 2, '/') == NULL && !strchr(p + 2, '.'); }

static int is_anim(const char* p) { return anim_dir_file(p) && !ends_with(p, "-"); }
static int is_tpl(const char* p) {
    return (anim_dir_file(p) && ends_with(p, "-")) || map_file(p, "t") || ends_with(p, ".tpl");
}
static int is_map(const char* p) { return map_file(p, "d"); }
static int is_camroad(const char* p) { return map_file(p, "c"); }
static int is_font(const char* p) { return ends_with(p, ".bfn"); }
static int is_icon_bin(const char* p) { return strcmp(p, "icon.bin") == 0; }
static int is_proj(const char* p) { return starts_with(p, "sound/") && ends_with(p, ".proj"); }
static int is_pool(const char* p) { return starts_with(p, "sound/") && ends_with(p, ".pool"); }
static int is_sdir(const char* p) { return starts_with(p, "sound/") && ends_with(p, ".sdir"); }
static int is_slib(const char* p) { return starts_with(p, "sound/") && ends_with(p, ".slib"); }
static int is_stm(const char* p) { return starts_with(p, "sound/") && ends_with(p, ".stm"); }

static void conv_icon_bin(void* data, uint32_t size, const char* name) {
    (void)name;
    portSwapU16Run(data, size / 2);
}
static void conv_proj(void* d, uint32_t n, const char* name) { (void)name, portMusyxSwapProj(d, n); }
static void conv_pool(void* d, uint32_t n, const char* name) { (void)name, portMusyxSwapPool(d, n); }
static void conv_sdir(void* d, uint32_t n, const char* name) { (void)name, portMusyxSwapSdir(d, n); }
static void conv_slib(void* d, uint32_t n, const char* name) { (void)name, portMusyxSwapSlib(d, n); }
static void conv_stm(void* d, uint32_t n, const char* name) { (void)n, (void)name, portMusyxSwapStreamHeader(d); }

/* A TPL reaches the game two ways: through filemgr with the file's size, and through UnpackTexPalette, which does
 * not know the size. Both run on every TPL. */
const HostFormat host_formats[] = {
    {"tpl", is_tpl, portTplSwap, 0, 0, 0, 1},
    {"tpl-nosize", is_tpl, portTplSwap, 0, 0, PORT_SWAP_SIZE_UNKNOWN, 1},
    {"anim", is_anim, portAnimSwap, 0, 0, 0, 1},
    {"map", is_map, portMapSwap, 0, 0, 0, 1},
    {"camroad", is_camroad, portCamRoadSwap, 0, 0, 0, 1},
    {"font", is_font, portFontSwap, 32, 0, 0, 1},
    {"icon.bin", is_icon_bin, conv_icon_bin, 0, 0, 0, 0},
    /* sound.c reads these rounded up to 32 bytes; each is converted once, as it is loaded */
    {"musyx-proj", is_proj, conv_proj, 32, 0, 0, 0},
    {"musyx-pool", is_pool, conv_pool, 32, 0, 0, 0},
    {"musyx-sdir", is_sdir, conv_sdir, 32, 0, 0, 0},
    {"musyx-slib", is_slib, conv_slib, 32, 0, 0, 0},
    /* a stream's header is the first block of its buffer (sound.c) */
    {"musyx-stm", is_stm, conv_stm, 0, 0x100, 0, 0},
};
const int host_format_count = (int)(sizeof(host_formats) / sizeof(host_formats[0]));
