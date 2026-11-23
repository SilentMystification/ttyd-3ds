/* fonttest=sjis: draws fixed Shift-JIS strings with the SDK's ROM font (OSInitFont, OSGetFontTexture, OSGetFontWidth
 * through the port's SJIS font, tools/port3ds/gen_sjis_font.py) over every frame, in any region build.
 * It switches the font encoding to SJIS only while loading and drawing, so the game's own text is unaffected.
 *
 * An event script with one looping user function queues the draw each frame (the display list is emptied at the start
 * of every frame, so a hook outside the game thread's frame cannot add to it); each glyph is one textured quad cut
 * from the 512x512 I4 sheet OSGetFontTexture names. Logs "fonttest: ..." lines.
 *   fonttest=sjis  draw the strings, 640x480 pixel coordinates
 */
#include <dolphin/gx.h>
#include <dolphin/mtx.h>
#include <dolphin/os.h>
#include <stdlib.h>
#include <string.h>

#include "driver/camdrv.h"
#include "driver/dispdrv.h"
#include "driver/seqdrv.h"
#include "evt/evt_cmd.h"
#include "manager/evtmgr.h"
#include "mario/mariost.h"
#include "memory.h"
#include "test_mode.h"

#define FONT_BYTES (3840 + 1179648)

static int s_state = -1; /* -1 unread, 0 off, 1 on */
static OSFontHeader* s_font;
/* From the C library heap, not the game arena: the test must not change the game's allocations (a 1.2 MB static array moved the game's boot out of what Azahar starts; the game arena made the title fail to load). */
static u8* s_font_mem;
static u32 s_ticks;       /* user function calls: stale = the script died with its map */
static u32 s_checked_at;
static u32 s_spawns;
static EvtWord s_script[4];

/* "マリオ　ストーリー　千年の扉　０１２", hiragana/katakana/half-width/ASCII, kanji, as Shift-JIS bytes. */
static const char* const s_lines[] = {
    "\203}\203\212\203I\201@\203X\203g\201[\203\212\201[\201@\220\347\224N\202\314\224\340\201@\202O\202P\202Q",
    "\202\320\202\347\202\252\202\310\201@\203J\203^\203J\203i\201@\312\335\266\270\266\305\201@ABC abc 123 !?",
    "\216\206\202\314\227E\216\322\201@\210\237\210\240\210\241\210\242\210\243\210\244\210\245\201@\212\277\216\232\201@\214\352",
};
#define LINES 3

static void draw_string(f32 x, f32 y, f32 scale, const char* s) {
    while (*s != '\0') {
        void* image;
        s32 sx, sy, width;
        const char* next = OSGetFontTexture(s, &image, &sx, &sy, &width);
        GXTexObj tex;
        f32 u0 = (f32)sx / 512.0f, u1 = (f32)(sx + 24) / 512.0f;
        f32 v0 = (f32)sy / 512.0f, v1 = (f32)(sy + 24) / 512.0f;
        f32 x1 = x + 24.0f * scale, y1 = y + 24.0f * scale;
        int pass;
        GXInitTexObj(&tex, image, 512, 512, GX_TF_I4, GX_CLAMP, GX_CLAMP, GX_FALSE);
        GXInitTexObjLOD(&tex, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
        GXLoadTexObj(&tex, GX_TEXMAP0);
        for (pass = 0; pass < 2; pass++) { /* a black copy one pixel down-right, then the white glyph */
            GXColor c = {0xFF, 0xFF, 0xFF, 0xFF};
            f32 o = pass == 0 ? 1.0f : 0.0f;
            if (pass == 0) {
                c.r = c.g = c.b = 0;
            }
            GXSetChanMatColor(GX_COLOR0A0, c);
            GXBegin(GX_QUADS, GX_VTXFMT0, 4);
            GXPosition3f32(x + o, y + o, -1.0f);
            GXTexCoord2f32(u0, v0);
            GXPosition3f32(x1 + o, y + o, -1.0f);
            GXTexCoord2f32(u1, v0);
            GXPosition3f32(x1 + o, y1 + o, -1.0f);
            GXTexCoord2f32(u1, v1);
            GXPosition3f32(x + o, y1 + o, -1.0f);
            GXTexCoord2f32(u0, v1);
            GXEnd();
        }
        x += (f32)width * scale;
        s = next;
    }
}

static void font_disp(CameraId cam, void* param) {
    Mtx44 proj;
    Mtx ident;
    u16 prev;
    int i;
    (void)cam;
    (void)param;
    if (s_font == NULL) {
        return;
    }
    prev = OSSetFontEncode(OS_FONT_ENCODE_SJIS);
    MTXOrtho(proj, 0.0f, 480.0f, 0.0f, 640.0f, 0.0f, 100.0f);
    GXSetProjection(proj, GX_ORTHOGRAPHIC);
    MTXIdentity(ident);
    GXLoadPosMtxImm(ident, GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
    GXSetViewport(0.0f, 0.0f, 640.0f, 480.0f, 0.0f, 1.0f);
    GXSetScissor(0, 0, 640, 480);
    GXSetCullMode(GX_CULL_NONE);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    GXSetZCompLoc(GX_TRUE);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GXSetZMode(GX_DISABLE, GX_LEQUAL, GX_DISABLE);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_NONE, GX_AF_NONE);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    GXSetNumTexGens(1);
    GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY, GX_FALSE, GX_PTIDENTITY);
    draw_string(24.0f, 24.0f, 1.25f, s_lines[0]);
    for (i = 1; i < LINES; i++) {
        draw_string(24.0f, 24.0f + 50.0f + (f32)(i - 1) * 40.0f, 1.0f, s_lines[i]);
    }
    OSSetFontEncode(prev);
}

static s32 font_user(EventEntry* evt, BOOL isFirstCall) {
    (void)evt;
    (void)isFirstCall;
    s_ticks++;
    dispEntry(CAMERA_2D, 0.0f, 3, font_disp, NULL);
    return EVT_RETURN_BLOCK;
}

/* What the JP game does at boot (romFontMake): OSLoadFont's 2-bit sheets and OSGetFontTexel into 24x24 I4 tiles. Checks
 * them against the I4 sheets OSInitFont made, for one glyph of each kind. Needs s_font loaded with OSInitFont first. */
static void selftest_load(void) {
    static const char* const probes[] = {"\202\240", "\203}", "\220\347", "\224\340", "A", "g", "\261", "\201@"};
    OSFontHeader* lf = (OSFontHeader*)malloc(0x90EE4 + 32);
    int k, bad = 0;
    if (lf == NULL) {
        return;
    }
    OSLoadFont(lf, NULL);
    for (k = 0; k < (int)(sizeof(probes) / sizeof(probes[0])); k++) {
        u8 tex[24 * 24 / 2];
        void* sheet;
        s32 sx, sy, w1, w2;
        int x, y, diff = 0;
        memset(tex, 0, sizeof(tex));
        OSGetFontTexel(probes[k], tex, 0, 6, &w1);
        /* the I4 sheet and cell of the same glyph, from the font OSInitFont made */
        OSInitFont(s_font);
        OSGetFontTexture(probes[k], &sheet, &sx, &sy, &w2);
        for (y = 0; y < 24; y++) {
            for (x = 0; x < 24; x++) {
                const u8* a = tex + (y / 8) * (3 * 32) + (x / 8) * 32 + (y % 8) * 4 + (x % 8) / 2;
                const u8* b = (const u8*)sheet + ((sy + y) / 8) * (64 * 32) + ((sx + x) / 8) * 32 + ((sy + y) % 8) * 4 + ((sx + x) % 8) / 2;
                int va = (*a >> ((x & 1) ? 0 : 4)) & 15, vb = (*b >> (((sx + x) & 1) ? 0 : 4)) & 15;
                diff += va != vb;
            }
        }
        OSLoadFont(lf, NULL);
        if (diff != 0 || w1 != w2) {
            bad++;
        }
        tst_log("fonttest: glyph %d: OSGetFontTexel vs the I4 sheet %d pixels differ, width %d/%d", k, diff, (int)w1, (int)w2);
    }
    tst_log("fonttest: OSLoadFont/OSGetFontTexel %s", bad == 0 ? "match OSInitFont" : "DIFFER from OSInitFont");
    free(lf);
}

void tst_font_frame(u32 frame) {
    if (s_state < 0) {
        char v[16];
        s_state = tst_sw_value("fonttest", v, sizeof(v)) && strcmp(v, "sjis") == 0;
    }
    if (!s_state || gp == NULL) {
        return;
    }
    if (s_font == NULL) {
        u16 prev;
        s_font_mem = malloc(FONT_BYTES + 32);
        if (s_font_mem == NULL) {
            tst_log("fonttest: no memory for the SJIS font");
            s_state = 0;
            return;
        }
        s_font = (OSFontHeader*)(((u32)s_font_mem + 31) & ~31u);
        prev = OSSetFontEncode(OS_FONT_ENCODE_SJIS);
        OSInitFont(s_font);
        selftest_load();
        OSInitFont(s_font); /* the SDK keeps the last loaded font: draw from the I4 sheets */
        OSSetFontEncode(prev);
        tst_log("fonttest: SJIS font loaded, %ux%u cells, sheet %ux%u, %u sheets of %u columns", (unsigned)s_font->cellWidth,
                (unsigned)s_font->cellHeight, (unsigned)s_font->sheetWidth, (unsigned)s_font->sheetHeight,
                (unsigned)(s_font->sheetFullSize / s_font->sheetSize), (unsigned)s_font->sheetColumn);
    }
    /* (re)start the drawing script when it is not running: not yet, or it died with its map */
    if (frame - s_checked_at >= 3) {
        s_checked_at = frame;
        if (s_ticks == s_spawns && (seqGetSeq() == SEQ_TITLE || seqGetSeq() == SEQ_GAME)) {
            s_script[0] = EVT_CMD(1, OPCODE_USER_FUNC);
            s_script[1] = (EvtWord)font_user;
            s_script[2] = EVT_CMD(0, OPCODE_END_EVENT);
            s_script[3] = OPCODE_END_SCRIPT;
            if (evtEntry(s_script, 0, 0) != NULL) {
                tst_log("fonttest: drawing script started at frame %lu", (unsigned long)frame);
            }
        }
        s_spawns = s_ticks;
    }
}
