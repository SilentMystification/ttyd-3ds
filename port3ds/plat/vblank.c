/* Vertical blank thread: the GameCube retrace interrupt on the 3DS. */
#include "plat_internal.h"

static void (*s_handler)(void);
static Thread s_thread;
static volatile uint32_t s_count;

uint32_t plat_vblank_count(void) { return s_count; }

static void vblank_thread(void* arg) {
    (void)arg;
    plat_crash_install_thread();
    plat_thread_register("vblank", 16 * 1024);
    for (;;) {
        gspWaitForVBlank();
        s_count++;
        /* HOME is seen here up to a game frame before the game thread's aptMainLoop reacts. */
        if (aptShouldJumpToHome()) {
            plat_cpu_limit_home_pressed();
        }
        if (s_handler != NULL) {
            s_handler();
        }
    }
}

void plat_vblank_start(void (*handler)(void)) {
    s_handler = handler;
    if (s_thread != NULL) {
        return;
    }
    /* High priority: retrace handling is short and the game thread waits on it. */
    s_thread = threadCreate(vblank_thread, NULL, 16 * 1024, 0x20, 1, false);
    if (s_thread == NULL) {
        s_thread = threadCreate(vblank_thread, NULL, 16 * 1024, 0x20, 0, false);
    }
    if (s_thread == NULL) {
        plat_fatal("vblank: no thread");
    }
}
