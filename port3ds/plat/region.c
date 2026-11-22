/* The system settings a region's game reads: the system language. */
#include "plat/region.h"

#include "plat_internal.h"

int plat_system_language(void) {
    static int s_lang = -2;
    if (s_lang != -2) {
        return s_lang;
    }
    s_lang = -1;
    Result r = cfguInit();
    if (R_FAILED(r)) {
        PLOG_W("region: cfguInit failed %08lx", (unsigned long)r);
        return s_lang;
    }
    u8 lang = 0;
    r = CFGU_GetSystemLanguage(&lang);
    cfguExit();
    if (R_FAILED(r)) {
        PLOG_W("region: CFGU_GetSystemLanguage failed %08lx", (unsigned long)r);
        return s_lang;
    }
    s_lang = lang;
    return s_lang;
}
