/* The system settings a region's game reads (plat/region.c). */
#pragma once

/* The 3DS system language (CFG_LANGUAGE_*: 0 Japanese, 1 English, 2 French,
 * 3 German, 4 Italian, 5 Spanish, 6 Chinese, 7 Korean, 8 Dutch, 9 Portuguese,
 * 10 Russian, 11 Taiwanese), or -1 when the setting cannot be read. */
int plat_system_language(void);
