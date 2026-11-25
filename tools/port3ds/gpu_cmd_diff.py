#!/usr/bin/env python3
"""Decode and compare PICA200 command lists dumped by the port (plat/gpudump.c).

The dumps are written to sdmc:/3ds/ttyd/gpuhang/ by
  - the GPU hang dump (first 2 s fence timeout):  hang_f<frame>_r<seq>.bin / .txt
  - the switch gpudump=N (console or Azahar):     dump_f<N>_r<seq>[_p<k>].bin / .txt
Fetch from the console with:  libs/ctr-devkit/devupdate/devupdate.py --ip $N3DS_IP files get @/gpuhang/<name> -o <file>
(`files ls @/gpuhang` lists them). In an Azahar session the files are under the session's sdmc/3ds/ttyd/gpuhang/.

A list is little-endian u32 words. Each command is: first parameter word, header word, then `extra`
more parameter words, padded to an even word count.
  header: bits 0-15 register id, 16-19 byte mask, 20-27 extra parameter count, 31 consecutive writes
  (parameter k goes to register id + k).

Usage:
  gpu_cmd_diff.py dump.bin                    decode, print findings
  gpu_cmd_diff.py a.bin b.bin [-n 20]         compare two lists (e.g. console hang vs Azahar frame)
  options: --full (print every command), --addr (compare address registers too; by default their values
  are masked because the console and the emulator allocate different addresses), -n N (differences shown)

Findings flagged as invalid: unknown register, register span past the register file, byte mask 0, nonzero
padding word, list ends inside a command, an address outside linear memory (0x20000000-0x30000000 physical)
and VRAM (0x18000000-0x18600000), a misaligned buffer address (attribute buffers 16 bytes, others 8), an
index or attribute offset that leaves the buffer region, texture dimension 0 or beyond 1024.
"""
import argparse
import difflib
import struct
import sys

REG_NAMES = {
    0x0000: "0000", 0x0001: "0001", 0x0002: "0002", 0x0003: "0003", 0x0004: "0004", 0x0005: "0005", 0x0006: "0006",
    0x0007: "0007", 0x0008: "0008", 0x0009: "0009", 0x000A: "000A", 0x000B: "000B", 0x000C: "000C", 0x000D: "000D",
    0x000E: "000E", 0x000F: "000F", 0x0010: "FINALIZE", 0x0011: "0011", 0x0012: "0012", 0x0013: "0013",
    0x0014: "0014", 0x0015: "0015", 0x0016: "0016", 0x0017: "0017", 0x0018: "0018", 0x0019: "0019", 0x001A: "001A",
    0x001B: "001B", 0x001C: "001C", 0x001D: "001D", 0x001E: "001E", 0x001F: "001F", 0x0020: "0020", 0x0021: "0021",
    0x0022: "0022", 0x0023: "0023", 0x0024: "0024", 0x0025: "0025", 0x0026: "0026", 0x0027: "0027", 0x0028: "0028",
    0x0029: "0029", 0x002A: "002A", 0x002B: "002B", 0x002C: "002C", 0x002D: "002D", 0x002E: "002E", 0x002F: "002F",
    0x0030: "0030", 0x0031: "0031", 0x0032: "0032", 0x0033: "0033", 0x0034: "0034", 0x0035: "0035", 0x0036: "0036",
    0x0037: "0037", 0x0038: "0038", 0x0039: "0039", 0x003A: "003A", 0x003B: "003B", 0x003C: "003C", 0x003D: "003D",
    0x003E: "003E", 0x003F: "003F", 0x0040: "FACECULLING_CONFIG", 0x0041: "VIEWPORT_WIDTH", 0x0042: "VIEWPORT_INVW",
    0x0043: "VIEWPORT_HEIGHT", 0x0044: "VIEWPORT_INVH", 0x0045: "0045", 0x0046: "0046", 0x0047: "FRAGOP_CLIP",
    0x0048: "FRAGOP_CLIP_DATA0", 0x0049: "FRAGOP_CLIP_DATA1", 0x004A: "FRAGOP_CLIP_DATA2",
    0x004B: "FRAGOP_CLIP_DATA3", 0x004C: "004C", 0x004D: "DEPTHMAP_SCALE", 0x004E: "DEPTHMAP_OFFSET",
    0x004F: "SH_OUTMAP_TOTAL", 0x0050: "SH_OUTMAP_O0", 0x0051: "SH_OUTMAP_O1", 0x0052: "SH_OUTMAP_O2",
    0x0053: "SH_OUTMAP_O3", 0x0054: "SH_OUTMAP_O4", 0x0055: "SH_OUTMAP_O5", 0x0056: "SH_OUTMAP_O6", 0x0057: "0057",
    0x0058: "0058", 0x0059: "0059", 0x005A: "005A", 0x005B: "005B", 0x005C: "005C", 0x005D: "005D", 0x005E: "005E",
    0x005F: "005F", 0x0060: "0060", 0x0061: "EARLYDEPTH_FUNC", 0x0062: "EARLYDEPTH_TEST1", 0x0063: "EARLYDEPTH_CLEAR",
    0x0064: "SH_OUTATTR_MODE", 0x0065: "SCISSORTEST_MODE", 0x0066: "SCISSORTEST_POS", 0x0067: "SCISSORTEST_DIM",
    0x0068: "VIEWPORT_XY", 0x0069: "0069", 0x006A: "EARLYDEPTH_DATA", 0x006B: "006B", 0x006C: "006C",
    0x006D: "DEPTHMAP_ENABLE", 0x006E: "RENDERBUF_DIM", 0x006F: "SH_OUTATTR_CLOCK", 0x0070: "0070", 0x0071: "0071",
    0x0072: "0072", 0x0073: "0073", 0x0074: "0074", 0x0075: "0075", 0x0076: "0076", 0x0077: "0077", 0x0078: "0078",
    0x0079: "0079", 0x007A: "007A", 0x007B: "007B", 0x007C: "007C", 0x007D: "007D", 0x007E: "007E", 0x007F: "007F",
    0x0080: "TEXUNIT_CONFIG", 0x0081: "TEXUNIT0_BORDER_COLOR", 0x0082: "TEXUNIT0_DIM", 0x0083: "TEXUNIT0_PARAM",
    0x0084: "TEXUNIT0_LOD", 0x0085: "TEXUNIT0_ADDR1", 0x0086: "TEXUNIT0_ADDR2", 0x0087: "TEXUNIT0_ADDR3",
    0x0088: "TEXUNIT0_ADDR4", 0x0089: "TEXUNIT0_ADDR5", 0x008A: "TEXUNIT0_ADDR6", 0x008B: "TEXUNIT0_SHADOW",
    0x008C: "008C", 0x008D: "008D", 0x008E: "TEXUNIT0_TYPE", 0x008F: "LIGHTING_ENABLE0", 0x0090: "0090",
    0x0091: "TEXUNIT1_BORDER_COLOR", 0x0092: "TEXUNIT1_DIM", 0x0093: "TEXUNIT1_PARAM", 0x0094: "TEXUNIT1_LOD",
    0x0095: "TEXUNIT1_ADDR", 0x0096: "TEXUNIT1_TYPE", 0x0097: "0097", 0x0098: "0098", 0x0099: "TEXUNIT2_BORDER_COLOR",
    0x009A: "TEXUNIT2_DIM", 0x009B: "TEXUNIT2_PARAM", 0x009C: "TEXUNIT2_LOD", 0x009D: "TEXUNIT2_ADDR",
    0x009E: "TEXUNIT2_TYPE", 0x009F: "009F", 0x00A0: "00A0", 0x00A1: "00A1", 0x00A2: "00A2", 0x00A3: "00A3",
    0x00A4: "00A4", 0x00A5: "00A5", 0x00A6: "00A6", 0x00A7: "00A7", 0x00A8: "TEXUNIT3_PROCTEX0",
    0x00A9: "TEXUNIT3_PROCTEX1", 0x00AA: "TEXUNIT3_PROCTEX2", 0x00AB: "TEXUNIT3_PROCTEX3", 0x00AE: "00AE",
    0x00AF: "PROCTEX_LUT", 0x00B0: "PROCTEX_LUT_DATA0", 0x00B1: "PROCTEX_LUT_DATA1", 0x00B2: "PROCTEX_LUT_DATA2",
    0x00B3: "PROCTEX_LUT_DATA3", 0x00B4: "PROCTEX_LUT_DATA4", 0x00B5: "PROCTEX_LUT_DATA5",
    0x00B6: "PROCTEX_LUT_DATA6", 0x00B7: "PROCTEX_LUT_DATA7", 0x00B8: "00B8", 0x00B9: "00B9", 0x00BA: "00BA",
    0x00BB: "00BB", 0x00BC: "00BC", 0x00BD: "00BD", 0x00BE: "00BE", 0x00BF: "00BF", 0x00C0: "TEXENV0_SOURCE",
    0x00C1: "TEXENV0_OPERAND", 0x00C2: "TEXENV0_COMBINER", 0x00C3: "TEXENV0_COLOR", 0x00C4: "TEXENV0_SCALE",
    0x00C5: "00C5", 0x00C6: "00C6", 0x00C7: "00C7", 0x00C8: "TEXENV1_SOURCE", 0x00C9: "TEXENV1_OPERAND",
    0x00CA: "TEXENV1_COMBINER", 0x00CB: "TEXENV1_COLOR", 0x00CC: "TEXENV1_SCALE", 0x00CD: "00CD", 0x00CE: "00CE",
    0x00CF: "00CF", 0x00D0: "TEXENV2_SOURCE", 0x00D1: "TEXENV2_OPERAND", 0x00D2: "TEXENV2_COMBINER",
    0x00D3: "TEXENV2_COLOR", 0x00D4: "TEXENV2_SCALE", 0x00D5: "00D5", 0x00D6: "00D6", 0x00D7: "00D7",
    0x00D8: "TEXENV3_SOURCE", 0x00D9: "TEXENV3_OPERAND", 0x00DA: "TEXENV3_COMBINER", 0x00DB: "TEXENV3_COLOR",
    0x00DC: "TEXENV3_SCALE", 0x00DD: "00DD", 0x00DE: "00DE", 0x00DF: "00DF", 0x00E0: "TEXENV_UPDATE_BUFFER",
    0x00E1: "FOG_COLOR", 0x00E2: "00E2", 0x00E3: "00E3", 0x00E4: "GAS_ATTENUATION", 0x00E5: "GAS_ACCMAX",
    0x00E6: "FOG_LUT_INDEX", 0x00E7: "00E7", 0x00E8: "FOG_LUT_DATA0", 0x00E9: "FOG_LUT_DATA1",
    0x00EA: "FOG_LUT_DATA2", 0x00EB: "FOG_LUT_DATA3", 0x00EC: "FOG_LUT_DATA4", 0x00ED: "FOG_LUT_DATA5",
    0x00EE: "FOG_LUT_DATA6", 0x00EF: "FOG_LUT_DATA7", 0x00F0: "TEXENV4_SOURCE", 0x00F1: "TEXENV4_OPERAND",
    0x00F2: "TEXENV4_COMBINER", 0x00F3: "TEXENV4_COLOR", 0x00F4: "TEXENV4_SCALE", 0x00F5: "00F5", 0x00F6: "00F6",
    0x00F7: "00F7", 0x00F8: "TEXENV5_SOURCE", 0x00F9: "TEXENV5_OPERAND", 0x00FA: "TEXENV5_COMBINER",
    0x00FB: "TEXENV5_COLOR", 0x00FC: "TEXENV5_SCALE", 0x00FD: "TEXENV_BUFFER_COLOR", 0x00FE: "00FE", 0x00FF: "00FF",
    0x0100: "COLOR_OPERATION", 0x0101: "BLEND_FUNC", 0x0102: "LOGIC_OP", 0x0103: "BLEND_COLOR",
    0x0104: "FRAGOP_ALPHA_TEST", 0x0105: "STENCIL_TEST", 0x0106: "STENCIL_OP", 0x0107: "DEPTH_COLOR_MASK",
    0x0108: "0108", 0x0109: "0109", 0x010A: "010A", 0x010B: "010B", 0x010C: "010C", 0x010D: "010D", 0x010E: "010E",
    0x010F: "010F", 0x0110: "FRAMEBUFFER_INVALIDATE", 0x0111: "FRAMEBUFFER_FLUSH", 0x0112: "COLORBUFFER_READ",
    0x0113: "COLORBUFFER_WRITE", 0x0114: "DEPTHBUFFER_READ", 0x0115: "DEPTHBUFFER_WRITE",
    0x0116: "DEPTHBUFFER_FORMAT", 0x0117: "COLORBUFFER_FORMAT", 0x0118: "EARLYDEPTH_TEST2", 0x0119: "0119",
    0x011A: "011A", 0x011B: "FRAMEBUFFER_BLOCK32", 0x011C: "DEPTHBUFFER_LOC", 0x011D: "COLORBUFFER_LOC",
    0x011E: "FRAMEBUFFER_DIM", 0x011F: "011F", 0x0120: "GAS_LIGHT_XY", 0x0121: "GAS_LIGHT_Z",
    0x0122: "GAS_LIGHT_Z_COLOR", 0x0123: "GAS_LUT_INDEX", 0x0124: "GAS_LUT_DATA", 0x0125: "GAS_ACCMAX_FEEDBACK",
    0x0126: "GAS_DELTAZ_DEPTH", 0x0127: "0127", 0x0128: "0128", 0x0129: "0129", 0x012A: "012A", 0x012B: "012B",
    0x012C: "012C", 0x012D: "012D", 0x012E: "012E", 0x012F: "012F", 0x0130: "FRAGOP_SHADOW", 0x0131: "0131",
    0x0132: "0132", 0x0133: "0133", 0x0134: "0134", 0x0135: "0135", 0x0136: "0136", 0x0137: "0137", 0x0138: "0138",
    0x0139: "0139", 0x013A: "013A", 0x013B: "013B", 0x013C: "013C", 0x013D: "013D", 0x013E: "013E", 0x013F: "013F",
    0x0140: "LIGHT0_SPECULAR0", 0x0141: "LIGHT0_SPECULAR1", 0x0142: "LIGHT0_DIFFUSE", 0x0143: "LIGHT0_AMBIENT",
    0x0144: "LIGHT0_XY", 0x0145: "LIGHT0_Z", 0x0146: "LIGHT0_SPOTDIR_XY", 0x0147: "LIGHT0_SPOTDIR_Z", 0x0148: "0148",
    0x0149: "LIGHT0_CONFIG", 0x014A: "LIGHT0_ATTENUATION_BIAS", 0x014B: "LIGHT0_ATTENUATION_SCALE", 0x014C: "014C",
    0x014D: "014D", 0x014E: "014E", 0x014F: "014F", 0x0150: "LIGHT1_SPECULAR0", 0x0151: "LIGHT1_SPECULAR1",
    0x0152: "LIGHT1_DIFFUSE", 0x0153: "LIGHT1_AMBIENT", 0x0154: "LIGHT1_XY", 0x0155: "LIGHT1_Z",
    0x0156: "LIGHT1_SPOTDIR_XY", 0x0157: "LIGHT1_SPOTDIR_Z", 0x0158: "0158", 0x0159: "LIGHT1_CONFIG",
    0x015A: "LIGHT1_ATTENUATION_BIAS", 0x015B: "LIGHT1_ATTENUATION_SCALE", 0x015C: "015C", 0x015D: "015D",
    0x015E: "015E", 0x015F: "015F", 0x0160: "LIGHT2_SPECULAR0", 0x0161: "LIGHT2_SPECULAR1", 0x0162: "LIGHT2_DIFFUSE",
    0x0163: "LIGHT2_AMBIENT", 0x0164: "LIGHT2_XY", 0x0165: "LIGHT2_Z", 0x0166: "LIGHT2_SPOTDIR_XY",
    0x0167: "LIGHT2_SPOTDIR_Z", 0x0168: "0168", 0x0169: "LIGHT2_CONFIG", 0x016A: "LIGHT2_ATTENUATION_BIAS",
    0x016B: "LIGHT2_ATTENUATION_SCALE", 0x016C: "016C", 0x016D: "016D", 0x016E: "016E", 0x016F: "016F",
    0x0170: "LIGHT3_SPECULAR0", 0x0171: "LIGHT3_SPECULAR1", 0x0172: "LIGHT3_DIFFUSE", 0x0173: "LIGHT3_AMBIENT",
    0x0174: "LIGHT3_XY", 0x0175: "LIGHT3_Z", 0x0176: "LIGHT3_SPOTDIR_XY", 0x0177: "LIGHT3_SPOTDIR_Z", 0x0178: "0178",
    0x0179: "LIGHT3_CONFIG", 0x017A: "LIGHT3_ATTENUATION_BIAS", 0x017B: "LIGHT3_ATTENUATION_SCALE", 0x017C: "017C",
    0x017D: "017D", 0x017E: "017E", 0x017F: "017F", 0x0180: "LIGHT4_SPECULAR0", 0x0181: "LIGHT4_SPECULAR1",
    0x0182: "LIGHT4_DIFFUSE", 0x0183: "LIGHT4_AMBIENT", 0x0184: "LIGHT4_XY", 0x0185: "LIGHT4_Z",
    0x0186: "LIGHT4_SPOTDIR_XY", 0x0187: "LIGHT4_SPOTDIR_Z", 0x0188: "0188", 0x0189: "LIGHT4_CONFIG",
    0x018A: "LIGHT4_ATTENUATION_BIAS", 0x018B: "LIGHT4_ATTENUATION_SCALE", 0x018C: "018C", 0x018D: "018D",
    0x018E: "018E", 0x018F: "018F", 0x0190: "LIGHT5_SPECULAR0", 0x0191: "LIGHT5_SPECULAR1", 0x0192: "LIGHT5_DIFFUSE",
    0x0193: "LIGHT5_AMBIENT", 0x0194: "LIGHT5_XY", 0x0195: "LIGHT5_Z", 0x0196: "LIGHT5_SPOTDIR_XY",
    0x0197: "LIGHT5_SPOTDIR_Z", 0x0198: "0198", 0x0199: "LIGHT5_CONFIG", 0x019A: "LIGHT5_ATTENUATION_BIAS",
    0x019B: "LIGHT5_ATTENUATION_SCALE", 0x019C: "019C", 0x019D: "019D", 0x019E: "019E", 0x019F: "019F",
    0x01A0: "LIGHT6_SPECULAR0", 0x01A1: "LIGHT6_SPECULAR1", 0x01A2: "LIGHT6_DIFFUSE", 0x01A3: "LIGHT6_AMBIENT",
    0x01A4: "LIGHT6_XY", 0x01A5: "LIGHT6_Z", 0x01A6: "LIGHT6_SPOTDIR_XY", 0x01A7: "LIGHT6_SPOTDIR_Z", 0x01A8: "01A8",
    0x01A9: "LIGHT6_CONFIG", 0x01AA: "LIGHT6_ATTENUATION_BIAS", 0x01AB: "LIGHT6_ATTENUATION_SCALE", 0x01AC: "01AC",
    0x01AD: "01AD", 0x01AE: "01AE", 0x01AF: "01AF", 0x01B0: "LIGHT7_SPECULAR0", 0x01B1: "LIGHT7_SPECULAR1",
    0x01B2: "LIGHT7_DIFFUSE", 0x01B3: "LIGHT7_AMBIENT", 0x01B4: "LIGHT7_XY", 0x01B5: "LIGHT7_Z",
    0x01B6: "LIGHT7_SPOTDIR_XY", 0x01B7: "LIGHT7_SPOTDIR_Z", 0x01B8: "01B8", 0x01B9: "LIGHT7_CONFIG",
    0x01BA: "LIGHT7_ATTENUATION_BIAS", 0x01BB: "LIGHT7_ATTENUATION_SCALE", 0x01BC: "01BC", 0x01BD: "01BD",
    0x01BE: "01BE", 0x01BF: "01BF", 0x01C0: "LIGHTING_AMBIENT", 0x01C1: "01C1", 0x01C2: "LIGHTING_NUM_LIGHTS",
    0x01C3: "LIGHTING_CONFIG0", 0x01C4: "LIGHTING_CONFIG1", 0x01C5: "LIGHTING_LUT_INDEX", 0x01C6: "LIGHTING_ENABLE1",
    0x01C7: "01C7", 0x01C8: "LIGHTING_LUT_DATA0", 0x01C9: "LIGHTING_LUT_DATA1", 0x01CA: "LIGHTING_LUT_DATA2",
    0x01CB: "LIGHTING_LUT_DATA3", 0x01CC: "LIGHTING_LUT_DATA4", 0x01CD: "LIGHTING_LUT_DATA5",
    0x01CE: "LIGHTING_LUT_DATA6", 0x01CF: "LIGHTING_LUT_DATA7", 0x01D0: "LIGHTING_LUTINPUT_ABS",
    0x01D1: "LIGHTING_LUTINPUT_SELECT", 0x01D2: "LIGHTING_LUTINPUT_SCALE", 0x01D3: "01D3", 0x01D4: "01D4",
    0x01D5: "01D5", 0x01D6: "01D6", 0x01D7: "01D7", 0x01D8: "01D8", 0x01D9: "LIGHTING_LIGHT_PERMUTATION",
    0x01DA: "01DA", 0x01DB: "01DB", 0x01DC: "01DC", 0x01DD: "01DD", 0x01DE: "01DE", 0x01DF: "01DF", 0x01E0: "01E0",
    0x01E1: "01E1", 0x01E2: "01E2", 0x01E3: "01E3", 0x01E4: "01E4", 0x01E5: "01E5", 0x01E6: "01E6", 0x01E7: "01E7",
    0x01E8: "01E8", 0x01E9: "01E9", 0x01EA: "01EA", 0x01EB: "01EB", 0x01EC: "01EC", 0x01ED: "01ED", 0x01EE: "01EE",
    0x01EF: "01EF", 0x01F0: "01F0", 0x01F1: "01F1", 0x01F2: "01F2", 0x01F3: "01F3", 0x01F4: "01F4", 0x01F5: "01F5",
    0x01F6: "01F6", 0x01F7: "01F7", 0x01F8: "01F8", 0x01F9: "01F9", 0x01FA: "01FA", 0x01FB: "01FB", 0x01FC: "01FC",
    0x01FD: "01FD", 0x01FE: "01FE", 0x01FF: "01FF", 0x0200: "ATTRIBBUFFERS_LOC", 0x0201: "ATTRIBBUFFERS_FORMAT_LOW",
    0x0202: "ATTRIBBUFFERS_FORMAT_HIGH", 0x0203: "ATTRIBBUFFER0_OFFSET", 0x0204: "ATTRIBBUFFER0_CONFIG1",
    0x0205: "ATTRIBBUFFER0_CONFIG2", 0x0206: "ATTRIBBUFFER1_OFFSET", 0x0207: "ATTRIBBUFFER1_CONFIG1",
    0x0208: "ATTRIBBUFFER1_CONFIG2", 0x0209: "ATTRIBBUFFER2_OFFSET", 0x020A: "ATTRIBBUFFER2_CONFIG1",
    0x020B: "ATTRIBBUFFER2_CONFIG2", 0x020C: "ATTRIBBUFFER3_OFFSET", 0x020D: "ATTRIBBUFFER3_CONFIG1",
    0x020E: "ATTRIBBUFFER3_CONFIG2", 0x020F: "ATTRIBBUFFER4_OFFSET", 0x0210: "ATTRIBBUFFER4_CONFIG1",
    0x0211: "ATTRIBBUFFER4_CONFIG2", 0x0212: "ATTRIBBUFFER5_OFFSET", 0x0213: "ATTRIBBUFFER5_CONFIG1",
    0x0214: "ATTRIBBUFFER5_CONFIG2", 0x0215: "ATTRIBBUFFER6_OFFSET", 0x0216: "ATTRIBBUFFER6_CONFIG1",
    0x0217: "ATTRIBBUFFER6_CONFIG2", 0x0218: "ATTRIBBUFFER7_OFFSET", 0x0219: "ATTRIBBUFFER7_CONFIG1",
    0x021A: "ATTRIBBUFFER7_CONFIG2", 0x021B: "ATTRIBBUFFER8_OFFSET", 0x021C: "ATTRIBBUFFER8_CONFIG1",
    0x021D: "ATTRIBBUFFER8_CONFIG2", 0x021E: "ATTRIBBUFFER9_OFFSET", 0x021F: "ATTRIBBUFFER9_CONFIG1",
    0x0220: "ATTRIBBUFFER9_CONFIG2", 0x0221: "ATTRIBBUFFERA_OFFSET", 0x0222: "ATTRIBBUFFERA_CONFIG1",
    0x0223: "ATTRIBBUFFERA_CONFIG2", 0x0224: "ATTRIBBUFFERB_OFFSET", 0x0225: "ATTRIBBUFFERB_CONFIG1",
    0x0226: "ATTRIBBUFFERB_CONFIG2", 0x0227: "INDEXBUFFER_CONFIG", 0x0228: "NUMVERTICES", 0x0229: "GEOSTAGE_CONFIG",
    0x022A: "VERTEX_OFFSET", 0x022B: "022B", 0x022C: "022C", 0x022D: "POST_VERTEX_CACHE_NUM", 0x022E: "DRAWARRAYS",
    0x022F: "DRAWELEMENTS", 0x0230: "0230", 0x0231: "VTX_FUNC", 0x0232: "FIXEDATTRIB_INDEX",
    0x0233: "FIXEDATTRIB_DATA0", 0x0234: "FIXEDATTRIB_DATA1", 0x0235: "FIXEDATTRIB_DATA2", 0x0236: "0236",
    0x0237: "0237", 0x0238: "CMDBUF_SIZE0", 0x0239: "CMDBUF_SIZE1", 0x023A: "CMDBUF_ADDR0", 0x023B: "CMDBUF_ADDR1",
    0x023C: "CMDBUF_JUMP0", 0x023D: "CMDBUF_JUMP1", 0x023E: "023E", 0x023F: "023F", 0x0240: "0240", 0x0241: "0241",
    0x0242: "VSH_NUM_ATTR", 0x0243: "0243", 0x0244: "VSH_COM_MODE", 0x0245: "START_DRAW_FUNC0", 0x0246: "0246",
    0x0247: "0247", 0x0248: "0248", 0x0249: "0249", 0x024A: "VSH_OUTMAP_TOTAL1", 0x024B: "024B", 0x024C: "024C",
    0x024D: "024D", 0x024E: "024E", 0x024F: "024F", 0x0250: "0250", 0x0251: "VSH_OUTMAP_TOTAL2", 0x0252: "GSH_MISC0",
    0x0253: "GEOSTAGE_CONFIG2", 0x0254: "GSH_MISC1", 0x0255: "0255", 0x0256: "0256", 0x0257: "0257", 0x0258: "0258",
    0x0259: "0259", 0x025A: "025A", 0x025B: "025B", 0x025C: "025C", 0x025D: "025D", 0x025E: "PRIMITIVE_CONFIG",
    0x025F: "RESTART_PRIMITIVE", 0x0260: "0260", 0x0261: "0261", 0x0262: "0262", 0x0263: "0263", 0x0264: "0264",
    0x0265: "0265", 0x0266: "0266", 0x0267: "0267", 0x0268: "0268", 0x0269: "0269", 0x026A: "026A", 0x026B: "026B",
    0x026C: "026C", 0x026D: "026D", 0x026E: "026E", 0x026F: "026F", 0x0270: "0270", 0x0271: "0271", 0x0272: "0272",
    0x0273: "0273", 0x0274: "0274", 0x0275: "0275", 0x0276: "0276", 0x0277: "0277", 0x0278: "0278", 0x0279: "0279",
    0x027A: "027A", 0x027B: "027B", 0x027C: "027C", 0x027D: "027D", 0x027E: "027E", 0x027F: "027F",
    0x0280: "GSH_BOOLUNIFORM", 0x0281: "GSH_INTUNIFORM_I0", 0x0282: "GSH_INTUNIFORM_I1", 0x0283: "GSH_INTUNIFORM_I2",
    0x0284: "GSH_INTUNIFORM_I3", 0x0285: "0285", 0x0286: "0286", 0x0287: "0287", 0x0288: "0288",
    0x0289: "GSH_INPUTBUFFER_CONFIG", 0x028A: "GSH_ENTRYPOINT", 0x028B: "GSH_ATTRIBUTES_PERMUTATION_LOW",
    0x028C: "GSH_ATTRIBUTES_PERMUTATION_HIGH", 0x028D: "GSH_OUTMAP_MASK", 0x028E: "028E",
    0x028F: "GSH_CODETRANSFER_END", 0x0290: "GSH_FLOATUNIFORM_CONFIG", 0x0291: "GSH_FLOATUNIFORM_DATA",
    0x0299: "0299", 0x029A: "029A", 0x029B: "GSH_CODETRANSFER_CONFIG", 0x029C: "GSH_CODETRANSFER_DATA",
    0x02A4: "02A4", 0x02A5: "GSH_OPDESCS_CONFIG", 0x02A6: "GSH_OPDESCS_DATA", 0x02AE: "02AE", 0x02AF: "02AF",
    0x02B0: "VSH_BOOLUNIFORM", 0x02B1: "VSH_INTUNIFORM_I0", 0x02B2: "VSH_INTUNIFORM_I1", 0x02B3: "VSH_INTUNIFORM_I2",
    0x02B4: "VSH_INTUNIFORM_I3", 0x02B5: "02B5", 0x02B6: "02B6", 0x02B7: "02B7", 0x02B8: "02B8",
    0x02B9: "VSH_INPUTBUFFER_CONFIG", 0x02BA: "VSH_ENTRYPOINT", 0x02BB: "VSH_ATTRIBUTES_PERMUTATION_LOW",
    0x02BC: "VSH_ATTRIBUTES_PERMUTATION_HIGH", 0x02BD: "VSH_OUTMAP_MASK", 0x02BE: "02BE",
    0x02BF: "VSH_CODETRANSFER_END", 0x02C0: "VSH_FLOATUNIFORM_CONFIG", 0x02C1: "VSH_FLOATUNIFORM_DATA",
    0x02C9: "02C9", 0x02CA: "02CA", 0x02CB: "VSH_CODETRANSFER_CONFIG", 0x02CC: "VSH_CODETRANSFER_DATA",
    0x02D4: "02D4", 0x02D5: "VSH_OPDESCS_CONFIG", 0x02D6: "VSH_OPDESCS_DATA", 0x02DE: "02DE", 0x02DF: "02DF",
    0x02E0: "02E0", 0x02E1: "02E1", 0x02E2: "02E2", 0x02E3: "02E3", 0x02E4: "02E4", 0x02E5: "02E5", 0x02E6: "02E6",
    0x02E7: "02E7", 0x02E8: "02E8", 0x02E9: "02E9", 0x02EA: "02EA", 0x02EB: "02EB", 0x02EC: "02EC", 0x02ED: "02ED",
    0x02EE: "02EE", 0x02EF: "02EF", 0x02F0: "02F0", 0x02F1: "02F1", 0x02F2: "02F2", 0x02F3: "02F3", 0x02F4: "02F4",
    0x02F5: "02F5", 0x02F6: "02F6", 0x02F7: "02F7", 0x02F8: "02F8", 0x02F9: "02F9", 0x02FA: "02FA", 0x02FB: "02FB",
    0x02FC: "02FC", 0x02FD: "02FD", 0x02FE: "02FE", 0x02FF: "02FF",
}

LINEAR = (0x20000000, 0x30000000)
VRAM = (0x18000000, 0x18600000)

# registers whose value is an address >> 3, with their required alignment in bytes (filled from the names)
LOC_NAMES = ("COLORBUFFER_LOC", "DEPTHBUFFER_LOC", "ATTRIBBUFFERS_LOC", "TEXUNIT0_ADDR1", "TEXUNIT0_ADDR2",
             "TEXUNIT0_ADDR3", "TEXUNIT0_ADDR4", "TEXUNIT0_ADDR5", "TEXUNIT0_ADDR6", "TEXUNIT1_ADDR", "TEXUNIT2_ADDR",
             "TEXUNIT0_SHADOWCUBE_ADDR", "TEXUNIT0_ADDR", "VSH_CODETRANSFER_ADDR", "FRAMEBUFFER_ADDR")
NAME_TO_ID = {v: k for k, v in REG_NAMES.items()}
ADDR_REGS = {}
for n in LOC_NAMES:
    if n in NAME_TO_ID:
        ADDR_REGS[NAME_TO_ID[n]] = 16 if n == "ATTRIBBUFFERS_LOC" else 8
for k, v in REG_NAMES.items():
    if v in ("TEXUNIT0_ADDR1", "TEXUNIT1_ADDR", "TEXUNIT2_ADDR") or v.startswith("TEXUNIT0_ADDR"):
        ADDR_REGS[k] = 8
ATTR_OFFSET_REGS = {NAME_TO_ID[n]: 16 for n in REG_NAMES.values() if n.startswith("ATTRIBBUFFER") and n.endswith("_OFFSET")}
IDXBUF = NAME_TO_ID.get("INDEXBUFFER_CONFIG")
REG_MAX = 0x300


def name_of(r):
    """The register's name; the 8-register data ports (uniform data, code and operand descriptor transfer) only
    name their first register in libctru, the others are shown as NAME+k."""
    n = REG_NAMES.get(r)
    if n is not None:
        return n
    for k in range(1, 8):
        b = REG_NAMES.get(r - k)
        if b is not None and b.endswith(("_DATA", "_DATA0")):
            return "%s+%d" % (b, k)
    return None


class Cmd:
    __slots__ = ("pos", "reg", "mask", "extra", "consec", "params", "bad")

    def __init__(self, pos, reg, mask, extra, consec, params):
        self.pos, self.reg, self.mask, self.extra, self.consec, self.params = pos, reg, mask, extra, consec, params
        self.bad = []

    def span(self):
        n = len(self.params)
        return range(self.reg, self.reg + (n if self.consec else 1))


def load(path):
    data = open(path, "rb").read()
    if len(data) % 4:
        print(f"{path}: size {len(data)} is not a multiple of 4", file=sys.stderr)
        data = data[: len(data) // 4 * 4]
    return list(struct.unpack("<%dI" % (len(data) // 4), data))


def decode(words):
    cmds = []
    i = 0
    n = len(words)
    while i < n:
        if i + 1 >= n:
            c = Cmd(i, -1, 0, 0, 0, [words[i]])
            c.bad.append("list ends inside a command (one word left)")
            cmds.append(c)
            break
        p0, hdr = words[i], words[i + 1]
        reg, mask, extra, consec = hdr & 0xFFFF, (hdr >> 16) & 0xF, (hdr >> 20) & 0xFF, hdr >> 31
        c = Cmd(i, reg, mask, extra, consec, [p0])
        if (hdr >> 28) & 0x7:
            c.bad.append("header bits 28-30 set (%X)" % ((hdr >> 28) & 7))
        end = i + 2 + extra
        if end > n:
            c.bad.append("list ends inside a command (%d of %d parameter words)" % (n - i - 2, extra))
            c.params += words[i + 2 : n]
            cmds.append(c)
            break
        c.params += words[i + 2 : end]
        pad = (extra + 2) & 1
        if pad:
            if end < n and words[end] != 0:
                c.bad.append("nonzero padding word %08X" % words[end])
            end += 1
        cmds.append(c)
        i = end
    return cmds


def in_range(a, rng):
    return rng[0] <= a < rng[1]


def check(cmds):
    findings = []
    attr_base = None  # ATTRIBBUFFERS_LOC as a physical address
    for c in cmds:
        if c.reg < 0:
            findings.extend((c.pos, b) for b in c.bad)
            continue
        for b in c.bad:
            findings.append((c.pos, b))
        if c.mask == 0:
            findings.append((c.pos, "byte mask 0: the write does nothing (%s)" % reg_label(c.reg)))
        for k in c.span():
            if name_of(k) is None:
                findings.append((c.pos, "unknown register 0x%04X%s" % (k, " (in a consecutive write from 0x%04X)" % c.reg if k != c.reg else "")))
        if c.reg + (len(c.params) if c.consec else 1) > REG_MAX:
            findings.append((c.pos, "register span 0x%04X..0x%04X goes past the register file (0x%X)" % (c.reg, c.reg + len(c.params) - 1, REG_MAX)))
        for k, v in enumerate(c.params):
            r = c.reg + (k if c.consec else 0)
            if r in ADDR_REGS:
                v_addr = v << 3 & 0xFFFFFFFF
                if v == 0:
                    continue
                if v_addr % ADDR_REGS[r]:
                    findings.append((c.pos, "%s = %08X: address %08X is not %d-byte aligned" % (name_of(r), v, v_addr, ADDR_REGS[r])))
                if not (in_range(v_addr, LINEAR) or in_range(v_addr, VRAM)):
                    findings.append((c.pos, "%s = %08X: address %08X is outside linear memory and VRAM" % (name_of(r), v, v_addr)))
            if r == NAME_TO_ID.get("ATTRIBBUFFERS_LOC"):
                attr_base = (v << 3) & 0xFFFFFFFF
            if r in ATTR_OFFSET_REGS:
                if v % 16:
                    findings.append((c.pos, "%s = %08X: attribute buffer offset is not 16-byte aligned" % (name_of(r), v)))
                if attr_base is not None and v:
                    eff = (attr_base + v) & 0xFFFFFFFF
                    if not (in_range(eff, LINEAR) or in_range(eff, VRAM)):
                        findings.append((c.pos, "%s = %08X: attribute buffer at %08X (base %08X + offset) is outside linear memory and VRAM" % (name_of(r), v, eff, attr_base)))
            if IDXBUF is not None and r == IDXBUF:
                off = v & 0x0FFFFFFF
                if off & 1 and not (v >> 31):
                    findings.append((c.pos, "INDEXBUFFER_CONFIG = %08X: odd offset for 8-bit indices is fine, for 16-bit not (format bit 31 is 0)" % v))
    return findings


def reg_label(r):
    return "%s(0x%04X)" % (name_of(r) or "UNKNOWN", r)


def fmt_cmd(c):
    if c.reg < 0:
        return "@%d: <truncated> %s" % (c.pos, " ".join("%08X" % p for p in c.params))
    vals = " ".join("%08X" % p for p in c.params[:8])
    if len(c.params) > 8:
        vals += " ... (+%d)" % (len(c.params) - 8)
    return "@%d: %-34s mask %X%s  %s" % (c.pos, reg_label(c.reg), c.mask, " consec" if c.consec else "", vals)


def summary(cmds):
    draws = sum(1 for c in cmds if c.reg in (NAME_TO_ID.get("DRAWARRAYS"), NAME_TO_ID.get("DRAWELEMENTS")))
    return "%d commands, %d draw triggers" % (len(cmds), draws)


def key(c, with_addr):
    params = list(c.params)
    if not with_addr and c.reg in ADDR_REGS:
        params = ["<addr>" if p else 0 for p in params]
    return (c.reg, c.mask, c.consec, tuple(params))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a")
    ap.add_argument("b", nargs="?")
    ap.add_argument("-n", type=int, default=20, help="differences to print")
    ap.add_argument("--full", action="store_true")
    ap.add_argument("--addr", action="store_true", help="compare address registers too")
    args = ap.parse_args()
    rc = 0
    lists = []
    for path in [p for p in (args.a, args.b) if p]:
        words = load(path)
        cmds = decode(words)
        lists.append((path, cmds))
        print("== %s: %d words, %s" % (path, len(words), summary(cmds)))
        fs = check(cmds)
        if fs:
            rc = 1
            print("   %d suspicious writes:" % len(fs))
            for pos, msg in fs[:40]:
                print("   @%d: %s" % (pos, msg))
            if len(fs) > 40:
                print("   ... %d more" % (len(fs) - 40))
        else:
            print("   no invalid writes found")
        if args.full:
            for c in cmds:
                print("   " + fmt_cmd(c))
    if len(lists) == 2:
        (pa, ca), (pb, cb) = lists
        ka = [key(c, args.addr) for c in ca]
        kb = [key(c, args.addr) for c in cb]
        sm = difflib.SequenceMatcher(None, ka, kb, autojunk=False)
        shown = 0
        same = True
        for tag, i1, i2, j1, j2 in sm.get_opcodes():
            if tag == "equal":
                continue
            same = False
            print("-- %s: A[%d:%d] B[%d:%d]" % (tag, i1, i2, j1, j2))
            for c in ca[i1:i2][:6]:
                print("   A " + fmt_cmd(c))
            for c in cb[j1:j2][:6]:
                print("   B " + fmt_cmd(c))
            shown += 1
            if shown >= args.n:
                print("... more differences not shown (-n)")
                break
        if same:
            print("== the two lists are identical%s" % ("" if args.addr else " (address register values masked)"))
    return rc


if __name__ == "__main__":
    sys.exit(main())
