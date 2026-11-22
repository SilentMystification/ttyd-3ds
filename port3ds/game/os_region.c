/* The IPL settings the PAL release reads (OSRtc): the language and the
 * 50/60 Hz choice.
 *
 * Language. marioStInit (G8MP01) picks its text from OSGetLanguage, the
 * language set in the GameCube's IPL menu. The port maps the 3DS system
 * language onto the five the game has (English, German, French, Spanish,
 * Italian); Dutch is passed as Dutch, which the game shows in English, as on
 * the GameCube; every other 3DS language gets English. Switch lang=<en|de|fr|
 * es|it|nl> (or 0..5, the OS_LANG_* number) overrides it. G8ME01 sets English
 * and G8MJ01 Japanese without asking (the JP font is SJIS, os_font.c).
 *
 * EURGB60. On the GameCube the PAL release's start-up prompt (seq_logo.c)
 * preselects the 50/60 Hz choice saved in SRAM and saves the new one. The
 * port keeps it for the session: switch hz=50 preselects 50 Hz, anything else
 * 60 Hz, the 3DS screen's own rate. The rate itself follows the render mode
 * the game configures (vi.c).
 */
#include <dolphin/os.h>
#include <string.h>

#include "plat/plat.h"
#include "plat/region.h"

/* CFG_LANGUAGE_* -> OS_LANG_* */
static u8 map_3ds_language(int lang) {
    switch (lang) {
    case 2: return OS_LANG_FRENCH;
    case 3: return OS_LANG_GERMAN;
    case 4: return OS_LANG_ITALIAN;
    case 5: return OS_LANG_SPANISH;
    case 8: return OS_LANG_DUTCH;
    default: return OS_LANG_ENGLISH;
    }
}

static int switch_language(void) {
    static const char* const names[] = {"en", "de", "fr", "es", "it", "nl"};
    char v[8];
    if (!plat_switch_value("lang", v, sizeof(v)) || v[0] == '\0') {
        return -1;
    }
    for (int i = 0; i < 6; i++) {
        if (strcmp(v, names[i]) == 0) {
            return i;
        }
    }
    if (v[0] >= '0' && v[0] <= '5' && v[1] == '\0') {
        return v[0] - '0';
    }
    PLOG_W("lang=%s: not one of en de fr es it nl (0..5)", v);
    return -1;
}

u8 OSGetLanguage(void) {
    static int s_lang = -1;
    if (s_lang < 0) {
        int sys = plat_system_language();
        int sw = switch_language();
        s_lang = sw >= 0 ? sw : map_3ds_language(sys);
        PLOG_I("language: %d (3DS system language %d%s)", s_lang, sys, sw >= 0 ? ", switch lang=" : "");
    }
    return (u8)s_lang;
}

static int s_eurgb60 = -1;

u32 OSGetEuRgb60Mode(void) {
    if (s_eurgb60 < 0) {
        s_eurgb60 = plat_switch_int("hz", 60) == 50 ? 0 : 1;
    }
    return (u32)s_eurgb60;
}

void OSSetEuRgb60Mode(u32 on) {
    s_eurgb60 = on ? 1 : 0;
    PLOG_I("EURGB60: %s", on ? "60 Hz" : "50 Hz");
}
