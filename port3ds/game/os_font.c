/* Dolphin OS ROM font on the port.
 *
 * The GameCube keeps its system font in the IPL ROM, which is neither on
 * the disc nor redistributable. The port builds the same in-memory layout
 * (OSFontHeader, 2-bit sheet for OSLoadFont, I4 sheet for OSInitFont) from
 * its own 5x7 ASCII font, drawn 3x into 24x24 cells, so the SDK's sheet
 * arithmetic (OSGetFontTexel / OSGetFontTexture, copied below) works as on
 * the GameCube. TTYD draws it only for system messages (disc errors, the
 * progressive-scan prompt, the fatal error screen), which the port does not
 * show; romfont.c still builds its glyph table from it at boot.
 *
 * ANSI (G8ME01, G8MP01) is that 5x7 font. The Japanese release uses the SJIS
 * encoding: a font with the IPL's glyph set and layout (24x24 cells, nine
 * 512x512 sheets) rendered by tools/port3ds/gen_sjis_font.py from Noto Sans
 * CJK JP (SIL OFL), embedded as sjis_font_bin. OSSetFontEncode switches to it
 * in any build (the fonttest=sjis switch).
 */
#include <dolphin/os.h>
#include <string.h>

#include "plat/plat.h"
#include "version.h"

#define CELL 24
#define SHEET_W 512
#define SHEET_H 256
#define COLS (SHEET_W / CELL) /* 21 */
#define ROWS (SHEET_H / CELL) /* 10 */
#define FIRST 0x20
#define LAST (FIRST + COLS * ROWS - 1) /* 0xF1 */

/* The headers of the last OSLoadFont/OSInitFont per encoding: the SDK keeps them globally too. */
static OSFontHeader* s_font;      /* ANSI */
static OSFontHeader* s_font_sjis; /* SJIS */
static u16 s_encode = 0xFFFF;     /* current encoding; 0xFFFF until first asked */

/* The SJIS font (tools/port3ds/gen_sjis_font.py), embedded by CMakeLists.txt; empty when not generated. */
extern const u8 sjis_font_bin[];
extern const u8 sjis_font_bin_end[];
#define SJIS_SHEETS 9

/* 5x7 glyphs for 0x20..0x7E, one byte per row, bit 4 = leftmost column. */
static const u8 s_font5x7[95][7] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /* space */
    {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04}, /* ! */
    {0x0A, 0x0A, 0x0A, 0x00, 0x00, 0x00, 0x00}, /* " */
    {0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A}, /* # */
    {0x04, 0x0F, 0x14, 0x0E, 0x05, 0x1E, 0x04}, /* $ */
    {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03}, /* % */
    {0x0C, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0D}, /* & */
    {0x0C, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00}, /* ' */
    {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}, /* ( */
    {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}, /* ) */
    {0x00, 0x04, 0x15, 0x0E, 0x15, 0x04, 0x00}, /* * */
    {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}, /* + */
    {0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08}, /* , */
    {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}, /* - */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}, /* . */
    {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00}, /* / */
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, /* 0 */
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, /* 1 */
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, /* 2 */
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}, /* 3 */
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, /* 4 */
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}, /* 5 */
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, /* 6 */
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, /* 7 */
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, /* 8 */
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}, /* 9 */
    {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}, /* : */
    {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x04, 0x08}, /* ; */
    {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02}, /* < */
    {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00}, /* = */
    {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08}, /* > */
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}, /* ? */
    {0x0E, 0x11, 0x01, 0x0D, 0x15, 0x15, 0x0E}, /* @ */
    {0x0E, 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11}, /* A */
    {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}, /* B */
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}, /* C */
    {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}, /* D */
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, /* E */
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}, /* F */
    {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}, /* G */
    {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, /* H */
    {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}, /* I */
    {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}, /* J */
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, /* K */
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}, /* L */
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}, /* M */
    {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}, /* N */
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, /* O */
    {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}, /* P */
    {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}, /* Q */
    {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}, /* R */
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}, /* S */
    {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}, /* T */
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, /* U */
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}, /* V */
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}, /* W */
    {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}, /* X */
    {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04}, /* Y */
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}, /* Z */
    {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E}, /* [ */
    {0x00, 0x10, 0x08, 0x04, 0x02, 0x01, 0x00}, /* backslash */
    {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E}, /* ] */
    {0x04, 0x0A, 0x11, 0x00, 0x00, 0x00, 0x00}, /* ^ */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F}, /* _ */
    {0x08, 0x04, 0x02, 0x00, 0x00, 0x00, 0x00}, /* ` */
    {0x00, 0x00, 0x0E, 0x01, 0x0F, 0x11, 0x0F}, /* a */
    {0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x1E}, /* b */
    {0x00, 0x00, 0x0E, 0x10, 0x10, 0x11, 0x0E}, /* c */
    {0x01, 0x01, 0x0D, 0x13, 0x11, 0x11, 0x0F}, /* d */
    {0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E}, /* e */
    {0x06, 0x09, 0x08, 0x1C, 0x08, 0x08, 0x08}, /* f */
    {0x00, 0x0F, 0x11, 0x11, 0x0F, 0x01, 0x0E}, /* g */
    {0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x11}, /* h */
    {0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E}, /* i */
    {0x02, 0x00, 0x06, 0x02, 0x02, 0x12, 0x0C}, /* j */
    {0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12}, /* k */
    {0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}, /* l */
    {0x00, 0x00, 0x1A, 0x15, 0x15, 0x11, 0x11}, /* m */
    {0x00, 0x00, 0x16, 0x19, 0x11, 0x11, 0x11}, /* n */
    {0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E}, /* o */
    {0x00, 0x00, 0x1E, 0x11, 0x1E, 0x10, 0x10}, /* p */
    {0x00, 0x00, 0x0D, 0x13, 0x0F, 0x01, 0x01}, /* q */
    {0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10}, /* r */
    {0x00, 0x00, 0x0E, 0x10, 0x0E, 0x01, 0x1E}, /* s */
    {0x08, 0x08, 0x1C, 0x08, 0x08, 0x09, 0x06}, /* t */
    {0x00, 0x00, 0x11, 0x11, 0x11, 0x13, 0x0D}, /* u */
    {0x00, 0x00, 0x11, 0x11, 0x11, 0x0A, 0x04}, /* v */
    {0x00, 0x00, 0x11, 0x11, 0x15, 0x15, 0x0A}, /* w */
    {0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11}, /* x */
    {0x00, 0x00, 0x11, 0x11, 0x0F, 0x01, 0x0E}, /* y */
    {0x00, 0x00, 0x1F, 0x02, 0x04, 0x08, 0x1F}, /* z */
    {0x02, 0x04, 0x04, 0x08, 0x04, 0x04, 0x02}, /* { */
    {0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}, /* | */
    {0x08, 0x04, 0x04, 0x02, 0x04, 0x04, 0x08}, /* } */
    {0x00, 0x00, 0x08, 0x15, 0x02, 0x00, 0x00}, /* ~ */
};

/* Pixel (x, y) of a 24x24 cell for code c: 2-bit value (0..3). */
static int cell_pixel(u32 c, int x, int y) {
    if (c < 0x20 || c > 0x7E) {
        c = 0x20;
    }
    int gx = (x - 4) / 3, gy = (y - 1) / 3;
    if (x < 4 || y < 1 || gx >= 5 || gy >= 7) {
        return 0;
    }
    return (s_font5x7[c - 0x20][gy] >> (4 - gx)) & 1 ? 3 : 0;
}

u16 OSGetFontEncode(void) {
    if (s_encode == 0xFFFF) {
#if VERSION == VERSION_JP
        s_encode = OS_FONT_ENCODE_SJIS;
#else
        s_encode = OS_FONT_ENCODE_ANSI;
#endif
    }
    return s_encode;
}

u16 OSSetFontEncode(u16 encode) {
    u16 prev = OSGetFontEncode();
    if (encode <= OS_FONT_ENCODE_MAX) {
        s_encode = encode;
    }
    return prev;
}

/* ---- SJIS ---- */

/* sjis_font_bin (tools/port3ds/gen_sjis_font.py): "SJF2", header bytes H, sheet bytes S, Zenkaku2Code count Z, zlib
 * size P (u32 each), Z u16 Zenkaku2Code, then the zlib stream of the OSFontHeader + widths (H) and 2-bit sheets (S). */
int uncompress(u8* dest, unsigned long* destLen, const u8* source, unsigned long sourceLen); /* zlib */

static u32 blob_u32(int i) {
    const u8* p = sjis_font_bin + 4 + i * 4;
    return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24;
}
#define BLOB_H blob_u32(0)
#define BLOB_S blob_u32(1)
#define BLOB_Z blob_u32(2)
#define BLOB_P blob_u32(3)

static int sjis_font_present(void) {
    return (u32)(sjis_font_bin_end - sjis_font_bin) > 20 && memcmp(sjis_font_bin, "SJF2", 4) == 0;
}

static void sjis_font_need(void) {
    if (!sjis_font_present()) {
        plat_fatal("the SJIS ROM font is missing: run tools/port3ds/gen_sjis_font.py and rebuild");
    }
}

/* Glyph index of a one- or two-byte Shift-JIS code: OSFont.c's GetFontCode. */
static int sjis_glyph(u32 code) {
    if (code >= 0x20 && code <= 0x7E) {
        return 0x20C + (int)(code - 0x20);
    }
    if (code >= 0xA1 && code <= 0xDF) {
        return 0x26B + (int)(code - 0xA1);
    }
    if (code < 0x100) {
        return 0x20C;
    }
    u32 trail = code & 0xFF;
    if (trail < 0x40 || trail > 0xFC || trail == 0x7F) {
        return 0;
    }
    int j = (int)trail - 0x40;
    if (j >= 0x40) {
        j--;
    }
    if (code > 0x889E && code <= 0x9872) {
        return 0x2BE + (int)((code >> 8) - 0x88) * 188 + j;
    }
    if (code >= 0x8140 && code < 0x879E) {
        u32 i = ((code >> 8) - 0x81) * 188 + (u32)j;
        if (i < BLOB_Z) {
            const u8* zen = sjis_font_bin + 20 + i * 2;
            return zen[0] | zen[1] << 8;
        }
    }
    return 0;
}

static int sjis_lead(u32 c) {
    return (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC);
}

static int sjis_trail(u32 c) {
    return c >= 0x40 && c <= 0xFC && c != 0x7F;
}

/* The font as OSLoadFont leaves it: header, widths and 2-bit sheets (sheetImage 0xEE4) in fontData. */
static u32 sjis_load(OSFontHeader* fontData) {
    sjis_font_need();
    s_font_sjis = fontData;
    unsigned long n = BLOB_H + BLOB_S;
    const u8* z = sjis_font_bin + 20 + BLOB_Z * 2;
    if (uncompress((u8*)fontData, &n, z, BLOB_P) != 0 || n != BLOB_H + BLOB_S) {
        plat_fatal("the SJIS ROM font does not unpack");
    }
    return fontData->sheetImage + BLOB_S;
}

/* As OSInitFont: the 2-bit sheets expanded to I4 through the colour table, in place (from the end, as the SDK's
 * ExpandFontSheet), the sheet moved up to a 32-byte boundary. */
static BOOL sjis_init(OSFontHeader* fontData) {
    sjis_load(fontData);
    u32 sbytes = BLOB_S;
    u8* src = (u8*)fontData + fontData->sheetImage;
    fontData->sheetImage = (fontData->sheetImage + 31) & ~31u;
    u8* dst = (u8*)fontData + fontData->sheetImage;
    const u8* ci = &fontData->c0;
    for (s32 i = (s32)sbytes - 1; i >= 0; i--) {
        u8 b = src[i];
        dst[i * 2 + 0] = (u8)((ci[b >> 6 & 3] & 0xF0) | (ci[b >> 4 & 3] & 0x0F));
        dst[i * 2 + 1] = (u8)((ci[b >> 2 & 3] & 0xF0) | (ci[b >> 0 & 3] & 0x0F));
    }
    return TRUE;
}

static void fill_header(OSFontHeader* h, u32 sheet_bytes) {
    memset(h, 0, sizeof(*h));
    h->fontType = 0;
    h->firstChar = FIRST;
    h->lastChar = LAST;
    h->invalChar = 0x20;
    h->ascent = 18;
    h->descent = 6;
    h->width = CELL;
    h->leading = 2;
    h->cellWidth = CELL;
    h->cellHeight = CELL;
    h->sheetSize = SHEET_W * SHEET_H;
    h->sheetFormat = GX_TF_I4;
    h->sheetColumn = COLS;
    h->sheetRow = ROWS;
    h->sheetWidth = SHEET_W;
    h->sheetHeight = SHEET_H;
    h->widthTable = sizeof(OSFontHeader);
    /* 48-byte header + 210 widths, 32-aligned: 288 bytes, as the SDK's
     * OS_FONT_SIZE_ANSI header part, so the I4 sheet ends exactly at the
     * 0x10120 bytes romfont.c allocates. */
    h->sheetImage = (sizeof(OSFontHeader) + COLS * ROWS + 31) & ~31u;
    h->sheetFullSize = sheet_bytes;
    h->c0 = 0x00;
    h->c1 = 0x55;
    h->c2 = 0xAA;
    h->c3 = 0xFF;
    u8* widths = (u8*)h + h->widthTable;
    for (int i = 0; i < COLS * ROWS; i++) {
        widths[i] = (FIRST + i >= 0x20 && FIRST + i <= 0x7E) ? 20 : 12;
    }
}

/* 2-bit sheet in the layout OSGetFontTexel reads (8x8 tiles, 16 bytes each). */
u32 OSLoadFont(OSFontHeader* fontData, void* tmp) {
    (void)tmp;
    if (OSGetFontEncode() == OS_FONT_ENCODE_SJIS) {
        return sjis_load(fontData);
    }
    s_font = fontData;
    u32 bytes = SHEET_W * SHEET_H / 4;
    fill_header(fontData, bytes);
    u8* sheet = (u8*)fontData + fontData->sheetImage;
    memset(sheet, 0, bytes);
    for (int code = FIRST; code <= LAST; code++) {
        int n = code - FIRST, row = (n / COLS) * CELL, col = (n % COLS) * CELL;
        for (int y = 0; y < CELL; y++) {
            for (int x = 0; x < CELL; x++) {
                int v = cell_pixel((u32)code, x, y);
                if (!v) {
                    continue;
                }
                int px = col + x, py = row + y;
                u8* p = sheet + ((SHEET_W / 8) * 32 / 2) * (py / 8) + (px / 8) * 16 + (py % 8) * 2 + (px % 8) / 4;
                *p |= (u8)(v << (6 - (px % 4) * 2));
            }
        }
    }
    return fontData->sheetImage + bytes;
}

/* I4 sheet for OSGetFontTexture (GX texture, 8x8 tiles of 32 bytes). */
BOOL OSInitFont(OSFontHeader* fontData) {
    if (OSGetFontEncode() == OS_FONT_ENCODE_SJIS) {
        return sjis_init(fontData);
    }
    s_font = fontData;
    u32 bytes = SHEET_W * SHEET_H / 2;
    fill_header(fontData, bytes);
    u8* sheet = (u8*)fontData + fontData->sheetImage;
    memset(sheet, 0, bytes);
    const u8 levels[4] = {0x0, 0x5, 0xA, 0xF};
    for (int code = FIRST; code <= LAST; code++) {
        int n = code - FIRST, row = (n / COLS) * CELL, col = (n % COLS) * CELL;
        for (int y = 0; y < CELL; y++) {
            for (int x = 0; x < CELL; x++) {
                int v = cell_pixel((u32)code, x, y);
                int px = col + x, py = row + y;
                u8* p = sheet + (py / 8) * (SHEET_W / 8) * 32 + (px / 8) * 32 + (py % 8) * 4 + (px % 8) / 2;
                *p |= (u8)(levels[v] << ((px & 1) ? 0 : 4));
            }
        }
    }
    return TRUE;
}

static const char* parse(const char* s, OSFontHeader** font, int* code, OSFontHeader* hdr) {
    if (OSGetFontEncode() == OS_FONT_ENCODE_SJIS) {
        u32 c = (u8)*s;
        if (c != 0) {
            s++;
            if (sjis_lead(c) && sjis_trail((u8)*s)) {
                c = c << 8 | (u8)*s++;
            }
        }
        *font = s_font_sjis;
        *code = sjis_glyph(c);
        return s;
    }
    u32 c = (u8)*s;
    if (c == 0) {
        c = 0x20;
    } else {
        s++;
    }
    if (c < FIRST || c > LAST) {
        c = hdr->invalChar;
    }
    *font = hdr;
    *code = (int)(c - FIRST);
    return s;
}

char* OSGetFontTexel(const char* string, void* image, s32 pos, s32 stride, s32* width) {
    OSFontHeader* font;
    int fontCode;
    if (s_font == NULL && s_font_sjis == NULL) {
        plat_fatal("OSGetFontTexel before OSLoadFont");
    }
    string = parse(string, &font, &fontCode, s_font);
    const u8* colorIndex = &font->c0;
    int sheet = fontCode / (font->sheetColumn * font->sheetRow);
    int numChars = fontCode - (sheet * (font->sheetColumn * font->sheetRow));
    int row = numChars / font->sheetColumn;
    int column = numChars - (row * font->sheetColumn);
    row *= font->cellHeight;
    column *= font->cellWidth;
    const u8* imageSrc = (const u8*)font + font->sheetImage + ((sheet * font->sheetSize) >> 1);
    for (int y = 0; y < font->cellHeight; y++) {
        for (int x = 0; x < font->cellWidth; x++) {
            const u8* src = imageSrc + (((font->sheetWidth / 8) * 32) / 2) * ((row + y) / 8);
            src += ((column + x) / 8) * 16;
            src += ((row + y) % 8) * 2;
            src += ((column + x) % 8) / 4;
            int offsetSrc = (column + x) % 4;
            u8* dst = (u8*)image + ((y / 8) * (((stride * 4) / 8) * 32));
            dst += (((pos + x) / 8) * 32);
            dst += ((y % 8) * 4);
            dst += ((pos + x) % 8) / 2;
            int offsetDst = (pos + x) % 2;
            *dst |= colorIndex[*src >> (6 - (offsetSrc * 2)) & 3] & ((offsetDst != 0) ? 0x0F : 0xF0);
        }
    }
    if (width != NULL) {
        *width = ((const u8*)font + font->widthTable)[fontCode];
    }
    return (char*)string;
}

char* OSGetFontTexture(const char* string, void** image, s32* x, s32* y, s32* width) {
    OSFontHeader* font;
    int fontCode;
    if (s_font == NULL && s_font_sjis == NULL) {
        plat_fatal("OSGetFontTexture before OSInitFont");
    }
    string = parse(string, &font, &fontCode, s_font);
    int sheet = fontCode / (font->sheetColumn * font->sheetRow);
    *image = (u8*)font + font->sheetImage + font->sheetSize * sheet; /* I4: sheetSize bytes a sheet */
    int numChars = fontCode - sheet * (font->sheetColumn * font->sheetRow);
    *y = (numChars / font->sheetColumn) * font->cellHeight;
    *x = (numChars % font->sheetColumn) * font->cellWidth;
    if (width != NULL) {
        *width = ((const u8*)font + font->widthTable)[fontCode];
    }
    return (char*)string;
}
