/*******************************************************************************
 * Size: 16 px
 * Bpp: 1
 * Opts: --bpp 1 --size 16 --font C:/Users/pc/SquareLine/assets/AlibabaPuHuiTi-3-75-SemiBold.ttf -o C:/Users/pc/SquareLine/assets\ui_font_Font1.c --format lvgl -r 0x20-0x7f --symbols 暂停标记会议纪要等待中未关联开始结束录音川:：- --no-compress --no-prefilter
 ******************************************************************************/

#include "../ui.h"

#ifndef UI_FONT_FONT1
#define UI_FONT_FONT1 1
#endif

#if UI_FONT_FONT1

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+0020 " " */
    0x0,

    /* U+0021 "!" */
    0xff, 0xfc, 0xc,

    /* U+0022 "\"" */
    0xde, 0xf7, 0xad, 0x0,

    /* U+0023 "#" */
    0x11, 0x19, 0xbf, 0xe6, 0x62, 0x21, 0x10, 0x89,
    0xff, 0x66, 0x33, 0x11, 0x0,

    /* U+0024 "$" */
    0x10, 0x10, 0x7e, 0xd0, 0xd0, 0xd0, 0xf0, 0x7e,
    0x1f, 0x13, 0x13, 0x13, 0xfc, 0x10, 0x10,

    /* U+0025 "%" */
    0x70, 0x8d, 0x98, 0xd9, 0xd, 0xa0, 0xda, 0x7,
    0x5e, 0x7, 0x30, 0xb3, 0x13, 0x31, 0x33, 0x21,
    0xe0,

    /* U+0026 "&" */
    0x3c, 0x19, 0x86, 0x61, 0xb8, 0x3c, 0x1e, 0x6f,
    0x9b, 0x36, 0xc7, 0x31, 0xc7, 0xdc,

    /* U+0027 "'" */
    0xff, 0xc0,

    /* U+0028 "(" */
    0x31, 0x98, 0xcc, 0x63, 0x18, 0xc6, 0x30, 0xc6,
    0x18, 0xc0,

    /* U+0029 ")" */
    0x63, 0xc, 0x61, 0x8c, 0x63, 0x18, 0xc6, 0x63,
    0x31, 0x80,

    /* U+002A "*" */
    0x10, 0xa1, 0xf8, 0xc2, 0xc5, 0x0,

    /* U+002B "+" */
    0x18, 0x18, 0x18, 0xff, 0x18, 0x18, 0x18, 0x18,

    /* U+002C "," */
    0x6f, 0x60,

    /* U+002D "-" */
    0xf8,

    /* U+002E "." */
    0xc0,

    /* U+002F "/" */
    0xc, 0x30, 0x86, 0x18, 0x43, 0xc, 0x21, 0x84,
    0x10, 0xc0,

    /* U+0030 "0" */
    0x3c, 0x66, 0xc3, 0xc3, 0xc3, 0xc3, 0xc3, 0xc3,
    0xc3, 0x66, 0x3c,

    /* U+0031 "1" */
    0x7f, 0xc6, 0x31, 0x8c, 0x63, 0x18, 0xc6,

    /* U+0032 "2" */
    0x7c, 0xe, 0x6, 0x6, 0x6, 0xe, 0x1c, 0x38,
    0x70, 0xe0, 0xff,

    /* U+0033 "3" */
    0xfc, 0xc, 0x18, 0x77, 0x81, 0x81, 0x83, 0x6,
    0x1b, 0xe0,

    /* U+0034 "4" */
    0xe, 0xe, 0x1e, 0x36, 0x66, 0x66, 0xc6, 0xff,
    0x6, 0x6, 0x6,

    /* U+0035 "5" */
    0x7c, 0x83, 0x6, 0xf, 0xc1, 0xc1, 0x83, 0x6,
    0x1b, 0xe0,

    /* U+0036 "6" */
    0x1c, 0x30, 0x60, 0x40, 0xfe, 0xe7, 0xc3, 0xc3,
    0xc3, 0x66, 0x3c,

    /* U+0037 "7" */
    0xfe, 0xc, 0x18, 0x60, 0xc3, 0x6, 0x8, 0x30,
    0x61, 0x80,

    /* U+0038 "8" */
    0x7e, 0xc3, 0xc3, 0xc3, 0x3c, 0x66, 0xc3, 0xc3,
    0xc3, 0xe7, 0x3c,

    /* U+0039 "9" */
    0x3c, 0x66, 0xc3, 0xc3, 0xc3, 0xe7, 0x7f, 0x3,
    0x6, 0xc, 0x38,

    /* U+003A ":" */
    0xc0, 0x0, 0xc0,

    /* U+003B ";" */
    0x70, 0x0, 0x3, 0x36, 0xe0,

    /* U+003C "<" */
    0x3, 0x1f, 0x78, 0xe0, 0xc0, 0x78, 0x1e, 0x7,
    0x0,

    /* U+003D "=" */
    0xff, 0x0, 0x0, 0x0, 0xff,

    /* U+003E ">" */
    0xc0, 0xf8, 0x1e, 0x7, 0x3, 0x1e, 0x78, 0xe0,
    0x0,

    /* U+003F "?" */
    0xfc, 0xc, 0x18, 0x30, 0xe3, 0x8e, 0x18, 0x30,
    0x0, 0xc0,

    /* U+0040 "@" */
    0xf, 0xc0, 0xc3, 0x8c, 0xc, 0xcf, 0xbc, 0xc9,
    0xec, 0x4f, 0x62, 0x7b, 0x36, 0xce, 0xe6, 0x0,
    0x18, 0x0, 0x60, 0x81, 0xfc, 0x0,

    /* U+0041 "A" */
    0xe, 0x1, 0x60, 0x6c, 0xd, 0x81, 0x98, 0x63,
    0xc, 0x73, 0xfe, 0x60, 0xcc, 0x1f, 0x1, 0x80,

    /* U+0042 "B" */
    0xfe, 0xc3, 0xc3, 0xc3, 0xc7, 0xfc, 0xc7, 0xc3,
    0xc3, 0xc3, 0xfe,

    /* U+0043 "C" */
    0x1f, 0x60, 0x60, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0,
    0xc0, 0x60, 0x3f,

    /* U+0044 "D" */
    0xfe, 0x61, 0xb0, 0xf8, 0x3c, 0x1e, 0xf, 0x7,
    0x83, 0xc3, 0x61, 0xbf, 0x80,

    /* U+0045 "E" */
    0xff, 0x83, 0x6, 0xc, 0x1f, 0xf0, 0x60, 0xc1,
    0x83, 0xf8,

    /* U+0046 "F" */
    0xff, 0x83, 0x6, 0xc, 0x1f, 0xf0, 0x60, 0xc1,
    0x83, 0x0,

    /* U+0047 "G" */
    0x1f, 0xb0, 0x58, 0x18, 0xc, 0x6, 0x3f, 0x7,
    0x83, 0xc1, 0xb0, 0xcf, 0xe0,

    /* U+0048 "H" */
    0xc1, 0xe0, 0xf0, 0x78, 0x3f, 0xfe, 0xf, 0x7,
    0x83, 0xc1, 0xe0, 0xf0, 0x60,

    /* U+0049 "I" */
    0xff, 0xff, 0xfc,

    /* U+004A "J" */
    0x18, 0xc6, 0x31, 0x8c, 0x63, 0x18, 0xc6, 0x31,
    0xf8,

    /* U+004B "K" */
    0xc3, 0x63, 0x33, 0x99, 0x8d, 0x87, 0x83, 0x61,
    0x98, 0xce, 0x63, 0xb0, 0xc0,

    /* U+004C "L" */
    0xc1, 0x83, 0x6, 0xc, 0x18, 0x30, 0x60, 0xc1,
    0x83, 0xf8,

    /* U+004D "M" */
    0xe0, 0xfe, 0x3f, 0xc7, 0xe8, 0xbd, 0xb7, 0xb6,
    0xf3, 0x9e, 0x73, 0xce, 0x78, 0xf, 0x1, 0x80,

    /* U+004E "N" */
    0xe1, 0xf8, 0xfc, 0x7b, 0x3d, 0x9e, 0x6f, 0x37,
    0x8f, 0xc7, 0xe1, 0xf0, 0xe0,

    /* U+004F "O" */
    0x3f, 0x18, 0x6c, 0xf, 0x3, 0xc0, 0xf0, 0x3c,
    0xf, 0x3, 0xc0, 0xd8, 0x63, 0xf0,

    /* U+0050 "P" */
    0xfe, 0xc7, 0xc3, 0xc3, 0xc3, 0xc7, 0xfc, 0xc0,
    0xc0, 0xc0, 0xc0,

    /* U+0051 "Q" */
    0x3f, 0x18, 0x6c, 0xf, 0x3, 0xc0, 0xf0, 0x3c,
    0xf, 0x3, 0xc0, 0xd8, 0x63, 0xf0, 0x1c, 0x3,
    0x80,

    /* U+0052 "R" */
    0xfe, 0x63, 0xb0, 0xd8, 0x6c, 0x77, 0xf3, 0x31,
    0x8c, 0xc7, 0x61, 0xb0, 0xe0,

    /* U+0053 "S" */
    0x7d, 0x83, 0x7, 0xf, 0x8f, 0xc7, 0x83, 0x6,
    0xf, 0xf0,

    /* U+0054 "T" */
    0xff, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
    0x18, 0x18, 0x18,

    /* U+0055 "U" */
    0xc1, 0xe0, 0xf0, 0x78, 0x3c, 0x1e, 0xf, 0x7,
    0x83, 0xc1, 0xb1, 0x8f, 0x80,

    /* U+0056 "V" */
    0xe0, 0xcc, 0x19, 0x83, 0x38, 0xc3, 0x18, 0x63,
    0xe, 0xc0, 0xd8, 0x1b, 0x1, 0xc0, 0x38, 0x0,

    /* U+0057 "W" */
    0xe0, 0xc, 0xc7, 0x19, 0x8e, 0x33, 0x1c, 0x66,
    0x6d, 0xc6, 0xdb, 0xd, 0xb6, 0x1a, 0x6c, 0x3c,
    0x78, 0x78, 0xe0, 0x71, 0xc0,

    /* U+0058 "X" */
    0x60, 0xdc, 0x63, 0x38, 0x6c, 0x1e, 0x3, 0x81,
    0xf0, 0xec, 0x33, 0x9c, 0x66, 0xc,

    /* U+0059 "Y" */
    0xe1, 0xd8, 0x63, 0x30, 0xcc, 0x1e, 0x7, 0x80,
    0xc0, 0x30, 0xc, 0x3, 0x0, 0xc0,

    /* U+005A "Z" */
    0xff, 0x3, 0x81, 0x81, 0x81, 0xc0, 0xc0, 0xc0,
    0xe0, 0x60, 0x70, 0x3f, 0xe0,

    /* U+005B "[" */
    0xfc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xf0,

    /* U+005C "\\" */
    0xc0, 0x60, 0x60, 0x20, 0x30, 0x10, 0x18, 0x18,
    0xc, 0xc, 0x4, 0x6, 0x2, 0x3,

    /* U+005D "]" */
    0xf3, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0xf0,

    /* U+005E "^" */
    0x38, 0x51, 0xb3, 0x64, 0x78, 0xc0,

    /* U+005F "_" */
    0xff,

    /* U+0060 "`" */
    0xc6, 0x20,

    /* U+0061 "a" */
    0x3e, 0x7, 0x3, 0x3, 0x7f, 0xc3, 0xc3, 0xc7,
    0x7f,

    /* U+0062 "b" */
    0xc0, 0xc0, 0xc0, 0xfe, 0xe6, 0xc3, 0xc3, 0xc3,
    0xc3, 0xc3, 0xe6, 0xfe,

    /* U+0063 "c" */
    0x3e, 0xc3, 0x6, 0xc, 0x18, 0x30, 0x30, 0x3e,

    /* U+0064 "d" */
    0x3, 0x3, 0x3, 0x3f, 0x67, 0xc3, 0xc3, 0xc3,
    0xc3, 0xc3, 0xe7, 0x7f,

    /* U+0065 "e" */
    0x3c, 0x62, 0xc3, 0xc7, 0xff, 0xc0, 0xc0, 0x61,
    0x3f,

    /* U+0066 "f" */
    0x3d, 0x86, 0x3e, 0x61, 0x86, 0x18, 0x61, 0x86,
    0x18,

    /* U+0067 "g" */
    0x3f, 0x67, 0xc3, 0xc3, 0xc3, 0xc3, 0xc3, 0x67,
    0x7f, 0x3, 0x6, 0x7c,

    /* U+0068 "h" */
    0xc0, 0xc0, 0xc0, 0xfe, 0xe3, 0xc3, 0xc3, 0xc3,
    0xc3, 0xc3, 0xc3, 0xc3,

    /* U+0069 "i" */
    0xc3, 0xff, 0xff,

    /* U+006A "j" */
    0x30, 0x3, 0x33, 0x33, 0x33, 0x33, 0x33, 0xe0,

    /* U+006B "k" */
    0xc0, 0xc0, 0xc0, 0xc6, 0xcc, 0xdc, 0xd8, 0xf0,
    0xd8, 0xdc, 0xcc, 0xc6,

    /* U+006C "l" */
    0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xc7,

    /* U+006D "m" */
    0xfd, 0xec, 0x63, 0xc6, 0x3c, 0x63, 0xc6, 0x3c,
    0x63, 0xc6, 0x3c, 0x63, 0xc6, 0x30,

    /* U+006E "n" */
    0xfe, 0xe3, 0xc3, 0xc3, 0xc3, 0xc3, 0xc3, 0xc3,
    0xc3,

    /* U+006F "o" */
    0x3c, 0x66, 0xc3, 0xc3, 0xc3, 0xc3, 0xc3, 0x66,
    0x3c,

    /* U+0070 "p" */
    0xfe, 0xe6, 0xc3, 0xc3, 0xc3, 0xc3, 0xc3, 0xe6,
    0xfe, 0xc0, 0xc0, 0xc0,

    /* U+0071 "q" */
    0x3f, 0x67, 0xc3, 0xc3, 0xc3, 0xc3, 0xc3, 0xe7,
    0x7f, 0x3, 0x3, 0x3,

    /* U+0072 "r" */
    0xff, 0x31, 0x8c, 0x63, 0x18, 0xc0,

    /* U+0073 "s" */
    0x7f, 0xc, 0x3c, 0xfc, 0x70, 0xc3, 0xf8,

    /* U+0074 "t" */
    0x63, 0x3e, 0xc6, 0x31, 0x8c, 0x63, 0xe,

    /* U+0075 "u" */
    0xc3, 0xc3, 0xc3, 0xc3, 0xc3, 0xc3, 0xc3, 0xc7,
    0x7f,

    /* U+0076 "v" */
    0xc1, 0xb1, 0x98, 0xcc, 0x63, 0x61, 0xb0, 0xd8,
    0x38, 0x1c, 0x0,

    /* U+0077 "w" */
    0xe3, 0x1b, 0x38, 0xd9, 0xe6, 0xcf, 0x36, 0x5b,
    0x1e, 0x58, 0xf3, 0xc7, 0x9c, 0x38, 0xe0,

    /* U+0078 "x" */
    0x63, 0x39, 0x8d, 0x83, 0xc1, 0xc1, 0xf0, 0xd8,
    0xc6, 0xe3, 0x80,

    /* U+0079 "y" */
    0xc3, 0xf1, 0x98, 0xcc, 0x43, 0x61, 0xb0, 0xd0,
    0x38, 0x1c, 0xc, 0x6, 0xc, 0x0,

    /* U+007A "z" */
    0xfe, 0x1c, 0x70, 0xc3, 0x86, 0x18, 0x70, 0xfe,

    /* U+007B "{" */
    0x36, 0x66, 0x66, 0x6c, 0x66, 0x66, 0x66, 0x30,

    /* U+007C "|" */
    0xff, 0xff, 0xff, 0xfc,

    /* U+007D "}" */
    0xc6, 0x66, 0x66, 0x63, 0x66, 0x66, 0x66, 0xc0,

    /* U+007E "~" */
    0x79, 0xa7, 0xb1, 0xc0,

    /* U+4E2D "中" */
    0x6, 0x0, 0x30, 0x1, 0x81, 0xff, 0xfc, 0x61,
    0xe3, 0xf, 0x18, 0x78, 0xc3, 0xc6, 0x1f, 0xff,
    0xc1, 0x80, 0xc, 0x0, 0x60, 0x3, 0x0, 0x18,
    0x0,

    /* U+4F1A "会" */
    0x3, 0x80, 0x7, 0x0, 0x1b, 0x80, 0xe3, 0x83,
    0x83, 0x9f, 0xff, 0xc0, 0x0, 0x0, 0x0, 0x7f,
    0xfc, 0xc, 0x0, 0x31, 0x80, 0xe3, 0x83, 0x83,
    0x7, 0xff, 0x0, 0x2, 0x0,

    /* U+505C "停" */
    0x30, 0xc0, 0xff, 0xf2, 0x0, 0x19, 0xfe, 0x64,
    0x1b, 0x8f, 0x8a, 0xff, 0xca, 0x3, 0x28, 0xc,
    0x9f, 0xc2, 0xc, 0x8, 0x30, 0x23, 0xc0,

    /* U+5173 "关" */
    0x10, 0x60, 0x63, 0x1, 0x8c, 0x1f, 0xfc, 0x2,
    0x0, 0x8, 0x0, 0x20, 0x3f, 0xfe, 0x7, 0x0,
    0x1c, 0x1, 0xd8, 0xe, 0x38, 0xf0, 0x7b, 0x0,
    0x60,

    /* U+59CB "始" */
    0x20, 0x40, 0x41, 0x80, 0x83, 0x67, 0xec, 0xcf,
    0xd0, 0xcd, 0xff, 0xdb, 0x1, 0x26, 0x0, 0xc9,
    0xfc, 0xf2, 0x18, 0xe4, 0x31, 0xc8, 0x67, 0xd0,
    0xdc, 0xbf, 0xb0, 0x43, 0x0,

    /* U+5DDD "川" */
    0x33, 0x19, 0x98, 0xcc, 0xc6, 0x66, 0x33, 0x31,
    0x99, 0x8c, 0xcc, 0x66, 0x63, 0x33, 0x1b, 0x18,
    0xd8, 0xc7, 0xc6, 0x3c, 0x1, 0x80,

    /* U+5F00 "开" */
    0x7f, 0xf8, 0x21, 0x80, 0x86, 0x2, 0x18, 0x8,
    0x60, 0x21, 0x8f, 0xff, 0xc6, 0x18, 0x18, 0x60,
    0x61, 0x81, 0x6, 0xc, 0x18, 0x60, 0x61, 0x1,
    0x80,

    /* U+5F55 "录" */
    0x7f, 0xf0, 0x0, 0xc3, 0xff, 0x0, 0xc, 0x0,
    0x33, 0xff, 0xf6, 0x31, 0x8c, 0xee, 0x17, 0xe0,
    0x7f, 0x87, 0x37, 0xb0, 0xc7, 0x8f, 0x4,

    /* U+5F85 "待" */
    0x10, 0xc0, 0xc3, 0x6, 0x7f, 0xb0, 0x30, 0x70,
    0xc0, 0xff, 0xf6, 0x3, 0x38, 0xc, 0xef, 0xfc,
    0x90, 0xc2, 0x63, 0x8, 0xcc, 0x20, 0x30, 0x83,
    0xc2, 0xe, 0x0,

    /* U+6682 "暂" */
    0x30, 0x1b, 0xff, 0xe7, 0x98, 0x36, 0x60, 0xff,
    0xfc, 0x66, 0xcf, 0xf3, 0x26, 0x4c, 0x19, 0x30,
    0xff, 0xc2, 0x3, 0xf, 0xfc, 0x20, 0x30, 0xff,
    0xc2, 0x3, 0x0,

    /* U+672A "未" */
    0x1, 0x0, 0x2, 0x0, 0x4, 0x1, 0xff, 0xc0,
    0x10, 0x0, 0x20, 0x1f, 0xff, 0x1, 0xc0, 0x7,
    0xc0, 0x1a, 0xc0, 0xe4, 0xe3, 0x88, 0xe4, 0x10,
    0xc0, 0x20, 0x0,

    /* U+675F "束" */
    0x1, 0x0, 0x2, 0x1, 0xff, 0xf0, 0x8, 0x0,
    0x10, 0x7, 0xff, 0x8, 0x42, 0x10, 0x84, 0x3f,
    0xf8, 0x7, 0x0, 0x1f, 0x0, 0xeb, 0x87, 0x93,
    0xcc, 0x21, 0x80, 0x40, 0x0,

    /* U+6807 "标" */
    0x18, 0x0, 0x33, 0xf8, 0x60, 0x3, 0xf0, 0x1,
    0x80, 0x7, 0x7f, 0xcf, 0x8, 0x3e, 0x94, 0x79,
    0xad, 0xb2, 0x59, 0x6c, 0x90, 0xd1, 0x31, 0xa2,
    0x63, 0x5c, 0x6, 0x38, 0x0,

    /* U+7B49 "等" */
    0x30, 0xc1, 0xff, 0xfc, 0x83, 0x0, 0xc0, 0x7f,
    0xf8, 0xc, 0xf, 0xff, 0xc0, 0x18, 0x0, 0x61,
    0xff, 0xf1, 0x86, 0x3, 0x18, 0x5, 0xe0, 0x6,
    0x0,

    /* U+7EAA "纪" */
    0x10, 0x0, 0x33, 0xf8, 0xd0, 0x31, 0x30, 0x67,
    0xc0, 0xcf, 0x1, 0x86, 0x7f, 0x8, 0xc0, 0x3d,
    0x80, 0xfb, 0x0, 0x6, 0x18, 0x3c, 0x3f, 0xf8,
    0x6c, 0x3f, 0x80,

    /* U+7ED3 "结" */
    0x18, 0x60, 0x20, 0xc0, 0xcf, 0xf9, 0x23, 0x6,
    0xc6, 0xf, 0xc, 0x6, 0xff, 0x8, 0x0, 0x30,
    0x0, 0xff, 0xf9, 0x8c, 0x30, 0x18, 0x61, 0xf0,
    0xcf, 0xff, 0x80, 0xc3, 0x0,

    /* U+8054 "联" */
    0x1, 0x9, 0xfb, 0x31, 0x22, 0xc2, 0x5f, 0xe7,
    0x86, 0x9, 0xc, 0x12, 0x18, 0x25, 0xfe, 0x7b,
    0xfc, 0x90, 0xc1, 0x23, 0xc7, 0xe7, 0x8f, 0x99,
    0x81, 0xe1, 0xc2, 0x81, 0x0,

    /* U+8981 "要" */
    0xff, 0xfc, 0x12, 0x0, 0x48, 0x1f, 0xfe, 0x64,
    0x99, 0xff, 0xe0, 0x80, 0x3, 0x0, 0xff, 0xfc,
    0x61, 0x83, 0xec, 0x1, 0xf0, 0xfe, 0x7b, 0x0,
    0x20,

    /* U+8BAE "议" */
    0x0, 0x80, 0xc1, 0x90, 0xd9, 0x30, 0x93, 0x40,
    0x30, 0x9e, 0x63, 0xc, 0x46, 0x18, 0xd8, 0x30,
    0xf0, 0x68, 0xc0, 0xf3, 0xc1, 0xdd, 0xc3, 0x70,
    0xe0, 0x80, 0xc0,

    /* U+8BB0 "记" */
    0x20, 0x1, 0xcf, 0xe3, 0x81, 0x84, 0x6, 0x0,
    0x1b, 0xc0, 0x63, 0x3f, 0x8c, 0xc0, 0x33, 0x0,
    0xcc, 0x3, 0x70, 0xcf, 0xc3, 0x3b, 0xc, 0x8f,
    0xe0,

    /* U+97F3 "音" */
    0x3, 0x1, 0xff, 0xe1, 0x86, 0x6, 0x18, 0xff,
    0xfc, 0x0, 0x0, 0x0, 0xf, 0xfc, 0x20, 0x10,
    0xff, 0xc2, 0x1, 0x8, 0x4, 0x3f, 0xf0, 0x80,
    0x40,

    /* U+FF1A "：" */
    0xfc, 0x0, 0x3f
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 66, .box_w = 1, .box_h = 1, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 1, .adv_w = 94, .box_w = 2, .box_h = 11, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 4, .adv_w = 105, .box_w = 5, .box_h = 5, .ofs_x = 1, .ofs_y = 7},
    {.bitmap_index = 8, .adv_w = 154, .box_w = 9, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 21, .adv_w = 150, .box_w = 8, .box_h = 15, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 36, .adv_w = 212, .box_w = 12, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 53, .adv_w = 164, .box_w = 10, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 67, .adv_w = 61, .box_w = 2, .box_h = 5, .ofs_x = 1, .ofs_y = 7},
    {.bitmap_index = 69, .adv_w = 98, .box_w = 5, .box_h = 15, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 79, .adv_w = 98, .box_w = 5, .box_h = 15, .ofs_x = 0, .ofs_y = -3},
    {.bitmap_index = 89, .adv_w = 115, .box_w = 7, .box_h = 6, .ofs_x = 0, .ofs_y = 6},
    {.bitmap_index = 95, .adv_w = 146, .box_w = 8, .box_h = 8, .ofs_x = 1, .ofs_y = 1},
    {.bitmap_index = 103, .adv_w = 82, .box_w = 3, .box_h = 4, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 105, .adv_w = 113, .box_w = 5, .box_h = 1, .ofs_x = 1, .ofs_y = 5},
    {.bitmap_index = 106, .adv_w = 82, .box_w = 2, .box_h = 1, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 107, .adv_w = 128, .box_w = 6, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 117, .adv_w = 150, .box_w = 8, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 128, .adv_w = 150, .box_w = 5, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 135, .adv_w = 150, .box_w = 8, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 146, .adv_w = 150, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 156, .adv_w = 150, .box_w = 8, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 167, .adv_w = 150, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 177, .adv_w = 150, .box_w = 8, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 188, .adv_w = 150, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 198, .adv_w = 150, .box_w = 8, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 209, .adv_w = 150, .box_w = 8, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 220, .adv_w = 101, .box_w = 2, .box_h = 9, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 223, .adv_w = 101, .box_w = 4, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 228, .adv_w = 146, .box_w = 8, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 237, .adv_w = 146, .box_w = 8, .box_h = 5, .ofs_x = 1, .ofs_y = 2},
    {.bitmap_index = 242, .adv_w = 146, .box_w = 8, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 251, .adv_w = 113, .box_w = 7, .box_h = 11, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 261, .adv_w = 219, .box_w = 13, .box_h = 13, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 283, .adv_w = 180, .box_w = 11, .box_h = 11, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 299, .adv_w = 163, .box_w = 8, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 310, .adv_w = 154, .box_w = 8, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 321, .adv_w = 181, .box_w = 9, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 334, .adv_w = 143, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 344, .adv_w = 137, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 354, .adv_w = 177, .box_w = 9, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 367, .adv_w = 185, .box_w = 9, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 380, .adv_w = 76, .box_w = 2, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 383, .adv_w = 75, .box_w = 5, .box_h = 14, .ofs_x = -2, .ofs_y = -3},
    {.bitmap_index = 392, .adv_w = 165, .box_w = 9, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 405, .adv_w = 133, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 415, .adv_w = 221, .box_w = 11, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 431, .adv_w = 192, .box_w = 9, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 444, .adv_w = 191, .box_w = 10, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 458, .adv_w = 159, .box_w = 8, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 469, .adv_w = 191, .box_w = 10, .box_h = 13, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 486, .adv_w = 163, .box_w = 9, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 499, .adv_w = 147, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 509, .adv_w = 139, .box_w = 8, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 520, .adv_w = 184, .box_w = 9, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 533, .adv_w = 172, .box_w = 11, .box_h = 11, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 549, .adv_w = 239, .box_w = 15, .box_h = 11, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 570, .adv_w = 170, .box_w = 10, .box_h = 11, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 584, .adv_w = 164, .box_w = 10, .box_h = 11, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 598, .adv_w = 164, .box_w = 9, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 611, .adv_w = 81, .box_w = 4, .box_h = 15, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 619, .adv_w = 130, .box_w = 8, .box_h = 14, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 633, .adv_w = 81, .box_w = 4, .box_h = 15, .ofs_x = 0, .ofs_y = -3},
    {.bitmap_index = 641, .adv_w = 146, .box_w = 7, .box_h = 6, .ofs_x = 1, .ofs_y = 5},
    {.bitmap_index = 647, .adv_w = 128, .box_w = 8, .box_h = 1, .ofs_x = 0, .ofs_y = -3},
    {.bitmap_index = 648, .adv_w = 103, .box_w = 4, .box_h = 3, .ofs_x = 1, .ofs_y = 10},
    {.bitmap_index = 650, .adv_w = 158, .box_w = 8, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 659, .adv_w = 164, .box_w = 8, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 671, .adv_w = 127, .box_w = 7, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 679, .adv_w = 164, .box_w = 8, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 691, .adv_w = 149, .box_w = 8, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 700, .adv_w = 91, .box_w = 6, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 709, .adv_w = 164, .box_w = 8, .box_h = 12, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 721, .adv_w = 161, .box_w = 8, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 733, .adv_w = 72, .box_w = 2, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 736, .adv_w = 72, .box_w = 4, .box_h = 15, .ofs_x = -1, .ofs_y = -3},
    {.bitmap_index = 744, .adv_w = 141, .box_w = 8, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 756, .adv_w = 76, .box_w = 4, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 762, .adv_w = 240, .box_w = 12, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 776, .adv_w = 161, .box_w = 8, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 785, .adv_w = 158, .box_w = 8, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 794, .adv_w = 164, .box_w = 8, .box_h = 12, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 806, .adv_w = 164, .box_w = 8, .box_h = 12, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 818, .adv_w = 101, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 824, .adv_w = 124, .box_w = 6, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 831, .adv_w = 90, .box_w = 5, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 838, .adv_w = 160, .box_w = 8, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 847, .adv_w = 144, .box_w = 9, .box_h = 9, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 858, .adv_w = 220, .box_w = 13, .box_h = 9, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 873, .adv_w = 145, .box_w = 9, .box_h = 9, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 884, .adv_w = 144, .box_w = 9, .box_h = 12, .ofs_x = 0, .ofs_y = -3},
    {.bitmap_index = 898, .adv_w = 137, .box_w = 7, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 906, .adv_w = 85, .box_w = 4, .box_h = 15, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 914, .adv_w = 54, .box_w = 2, .box_h = 15, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 918, .adv_w = 85, .box_w = 4, .box_h = 15, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 926, .adv_w = 146, .box_w = 9, .box_h = 3, .ofs_x = 0, .ofs_y = 4},
    {.bitmap_index = 930, .adv_w = 252, .box_w = 13, .box_h = 15, .ofs_x = 2, .ofs_y = -2},
    {.bitmap_index = 955, .adv_w = 252, .box_w = 15, .box_h = 15, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 984, .adv_w = 252, .box_w = 14, .box_h = 13, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 1007, .adv_w = 252, .box_w = 14, .box_h = 14, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 1032, .adv_w = 252, .box_w = 15, .box_h = 15, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 1061, .adv_w = 252, .box_w = 13, .box_h = 13, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 1083, .adv_w = 252, .box_w = 14, .box_h = 14, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 1108, .adv_w = 252, .box_w = 14, .box_h = 13, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 1131, .adv_w = 252, .box_w = 14, .box_h = 15, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 1158, .adv_w = 252, .box_w = 14, .box_h = 15, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 1185, .adv_w = 252, .box_w = 15, .box_h = 14, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 1212, .adv_w = 252, .box_w = 15, .box_h = 15, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 1241, .adv_w = 252, .box_w = 15, .box_h = 15, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 1270, .adv_w = 252, .box_w = 14, .box_h = 14, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 1295, .adv_w = 252, .box_w = 15, .box_h = 14, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 1322, .adv_w = 252, .box_w = 15, .box_h = 15, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 1351, .adv_w = 252, .box_w = 15, .box_h = 15, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 1380, .adv_w = 252, .box_w = 14, .box_h = 14, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 1405, .adv_w = 252, .box_w = 15, .box_h = 14, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 1432, .adv_w = 252, .box_w = 14, .box_h = 14, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 1457, .adv_w = 252, .box_w = 14, .box_h = 14, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 1482, .adv_w = 252, .box_w = 3, .box_h = 8, .ofs_x = 3, .ofs_y = 0}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/

static const uint16_t unicode_list_1[] = {
    0x0, 0xed, 0x22f, 0x346, 0xb9e, 0xfb0, 0x10d3, 0x1128,
    0x1158, 0x1855, 0x18fd, 0x1932, 0x19da, 0x2d1c, 0x307d, 0x30a6,
    0x3227, 0x3b54, 0x3d81, 0x3d83, 0x49c6, 0xb0ed
};

/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 32, .range_length = 95, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    },
    {
        .range_start = 20013, .range_length = 45294, .glyph_id_start = 96,
        .unicode_list = unicode_list_1, .glyph_id_ofs_list = NULL, .list_length = 22, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY
    }
};

/*-----------------
 *    KERNING
 *----------------*/


/*Pair left and right glyphs for kerning*/
static const uint8_t kern_pair_glyph_ids[] =
{
    3, 3,
    3, 8,
    3, 13,
    3, 15,
    8, 3,
    8, 8,
    8, 13,
    8, 15,
    9, 75,
    13, 3,
    13, 8,
    13, 18,
    13, 24,
    13, 26,
    15, 3,
    15, 8,
    15, 18,
    15, 24,
    15, 26,
    16, 16,
    18, 13,
    18, 15,
    18, 27,
    18, 28,
    24, 13,
    24, 15,
    24, 27,
    24, 28,
    27, 18,
    27, 24,
    28, 18,
    28, 24,
    34, 3,
    34, 8,
    34, 36,
    34, 40,
    34, 48,
    34, 50,
    34, 53,
    34, 54,
    34, 55,
    34, 56,
    34, 58,
    34, 71,
    34, 77,
    34, 85,
    34, 87,
    34, 90,
    35, 36,
    35, 40,
    35, 48,
    35, 50,
    35, 53,
    35, 55,
    35, 57,
    35, 58,
    37, 13,
    37, 15,
    37, 34,
    37, 53,
    37, 55,
    37, 56,
    37, 57,
    37, 58,
    37, 59,
    37, 66,
    38, 75,
    39, 13,
    39, 15,
    39, 34,
    39, 66,
    39, 73,
    39, 76,
    40, 55,
    40, 58,
    43, 43,
    44, 34,
    44, 36,
    44, 40,
    44, 48,
    44, 50,
    44, 53,
    44, 54,
    44, 55,
    44, 56,
    44, 58,
    44, 68,
    44, 69,
    44, 70,
    44, 72,
    44, 77,
    44, 80,
    44, 82,
    44, 84,
    44, 85,
    44, 86,
    44, 87,
    44, 88,
    44, 90,
    45, 3,
    45, 8,
    45, 34,
    45, 36,
    45, 40,
    45, 48,
    45, 50,
    45, 53,
    45, 54,
    45, 55,
    45, 56,
    45, 58,
    45, 87,
    45, 88,
    45, 90,
    48, 13,
    48, 15,
    48, 34,
    48, 53,
    48, 55,
    48, 56,
    48, 57,
    48, 58,
    48, 59,
    48, 66,
    49, 13,
    49, 15,
    49, 34,
    49, 53,
    49, 55,
    49, 56,
    49, 57,
    49, 58,
    49, 59,
    49, 66,
    50, 13,
    50, 15,
    50, 34,
    50, 43,
    50, 53,
    50, 55,
    50, 56,
    50, 57,
    50, 58,
    50, 59,
    50, 66,
    51, 36,
    51, 40,
    51, 48,
    51, 50,
    51, 53,
    51, 54,
    51, 55,
    51, 56,
    51, 57,
    51, 58,
    51, 68,
    51, 69,
    51, 70,
    51, 72,
    51, 80,
    51, 82,
    51, 85,
    51, 86,
    51, 87,
    51, 89,
    51, 90,
    52, 13,
    52, 15,
    52, 52,
    52, 53,
    52, 55,
    52, 56,
    52, 58,
    52, 85,
    52, 87,
    52, 88,
    52, 89,
    52, 90,
    53, 3,
    53, 8,
    53, 13,
    53, 15,
    53, 34,
    53, 36,
    53, 40,
    53, 48,
    53, 50,
    53, 52,
    53, 66,
    53, 68,
    53, 69,
    53, 70,
    53, 72,
    53, 74,
    53, 75,
    53, 78,
    53, 79,
    53, 80,
    53, 81,
    53, 82,
    53, 83,
    53, 84,
    53, 85,
    53, 86,
    53, 87,
    53, 88,
    53, 89,
    53, 90,
    53, 91,
    54, 34,
    54, 57,
    54, 89,
    55, 13,
    55, 15,
    55, 27,
    55, 28,
    55, 34,
    55, 36,
    55, 40,
    55, 48,
    55, 50,
    55, 52,
    55, 66,
    55, 68,
    55, 69,
    55, 70,
    55, 72,
    55, 73,
    55, 76,
    55, 80,
    55, 82,
    55, 84,
    55, 87,
    55, 90,
    56, 13,
    56, 15,
    56, 27,
    56, 28,
    56, 34,
    56, 36,
    56, 40,
    56, 48,
    56, 50,
    56, 66,
    56, 68,
    56, 69,
    56, 70,
    56, 72,
    56, 80,
    56, 82,
    56, 84,
    57, 34,
    57, 36,
    57, 40,
    57, 48,
    57, 50,
    57, 53,
    57, 54,
    57, 55,
    57, 56,
    57, 58,
    57, 68,
    57, 69,
    57, 70,
    57, 72,
    57, 77,
    57, 80,
    57, 82,
    57, 84,
    57, 85,
    57, 86,
    57, 87,
    57, 88,
    57, 90,
    58, 3,
    58, 8,
    58, 13,
    58, 15,
    58, 27,
    58, 28,
    58, 34,
    58, 36,
    58, 40,
    58, 48,
    58, 50,
    58, 52,
    58, 66,
    58, 68,
    58, 69,
    58, 70,
    58, 71,
    58, 72,
    58, 73,
    58, 74,
    58, 76,
    58, 78,
    58, 79,
    58, 80,
    58, 81,
    58, 82,
    58, 83,
    58, 84,
    58, 85,
    58, 86,
    58, 87,
    58, 89,
    58, 90,
    58, 91,
    59, 36,
    59, 40,
    59, 48,
    59, 50,
    60, 75,
    66, 53,
    66, 55,
    66, 58,
    66, 87,
    66, 88,
    66, 90,
    67, 13,
    67, 15,
    67, 53,
    67, 55,
    67, 56,
    67, 57,
    67, 58,
    67, 87,
    67, 89,
    67, 90,
    70, 13,
    70, 15,
    70, 53,
    70, 58,
    70, 75,
    71, 3,
    71, 8,
    71, 10,
    71, 11,
    71, 13,
    71, 15,
    71, 32,
    71, 53,
    71, 55,
    71, 56,
    71, 57,
    71, 58,
    71, 62,
    71, 66,
    71, 68,
    71, 69,
    71, 70,
    71, 71,
    71, 72,
    71, 73,
    71, 74,
    71, 75,
    71, 76,
    71, 77,
    71, 80,
    71, 82,
    71, 84,
    71, 85,
    71, 87,
    71, 90,
    71, 94,
    73, 53,
    73, 58,
    73, 87,
    73, 88,
    73, 90,
    74, 3,
    74, 8,
    74, 10,
    74, 32,
    74, 53,
    74, 62,
    74, 94,
    75, 53,
    76, 53,
    76, 54,
    76, 56,
    76, 66,
    76, 68,
    76, 69,
    76, 70,
    76, 72,
    76, 77,
    76, 80,
    76, 82,
    76, 86,
    77, 71,
    77, 85,
    77, 87,
    77, 88,
    77, 90,
    78, 53,
    78, 58,
    78, 87,
    78, 88,
    78, 90,
    79, 53,
    79, 58,
    79, 87,
    79, 88,
    79, 90,
    80, 13,
    80, 15,
    80, 53,
    80, 55,
    80, 56,
    80, 57,
    80, 58,
    80, 87,
    80, 89,
    80, 90,
    81, 13,
    81, 15,
    81, 53,
    81, 55,
    81, 56,
    81, 57,
    81, 58,
    81, 87,
    81, 89,
    81, 90,
    82, 53,
    82, 58,
    82, 75,
    83, 3,
    83, 8,
    83, 13,
    83, 15,
    83, 66,
    83, 68,
    83, 69,
    83, 70,
    83, 71,
    83, 80,
    83, 82,
    83, 85,
    83, 87,
    83, 90,
    84, 53,
    84, 55,
    84, 56,
    84, 57,
    84, 58,
    84, 87,
    84, 89,
    84, 90,
    85, 71,
    85, 85,
    86, 53,
    86, 58,
    87, 13,
    87, 15,
    87, 53,
    87, 57,
    87, 66,
    87, 68,
    87, 69,
    87, 70,
    87, 80,
    87, 82,
    88, 13,
    88, 15,
    88, 53,
    88, 57,
    88, 66,
    89, 53,
    89, 54,
    89, 56,
    89, 66,
    89, 68,
    89, 69,
    89, 70,
    89, 72,
    89, 77,
    89, 80,
    89, 82,
    89, 86,
    90, 13,
    90, 15,
    90, 53,
    90, 57,
    90, 66,
    90, 68,
    90, 69,
    90, 70,
    90, 80,
    90, 82,
    91, 53,
    92, 73,
    92, 75,
    92, 76
};

/* Kerning between the respective left and right glyphs
 * 4.4 format which needs to scaled with `kern_scale`*/
static const int8_t kern_pair_values[] =
{
    -3, -7, -44, -44, -3, -7, -44, -44,
    15, -41, -41, -36, -14, -8, -41, -41,
    -36, -14, -8, -31, -22, -22, -24, -24,
    -41, -41, -22, -22, -18, -13, -18, -13,
    -26, -26, -4, -4, -4, -4, -20, -2,
    -18, -7, -23, -5, -3, -5, -5, -5,
    -2, -2, -2, -2, -9, -5, -5, -7,
    -13, -13, -5, -9, -8, -3, -6, -10,
    -5, -6, 3, -37, -37, -10, -13, 2,
    2, -5, -8, 5, -3, -6, -6, -6,
    -6, -3, -8, -6, -6, -10, -8, -8,
    -8, -8, -8, -8, -8, -3, -4, -9,
    -15, -10, -15, -24, -24, 5, -11, -11,
    -11, -11, -16, -3, -18, -8, -26, -12,
    -7, -12, -13, -13, -5, -9, -8, -3,
    -6, -10, -5, -6, -47, -47, -15, -5,
    -2, -2, -13, -5, -8, -15, -13, -13,
    -5, 5, -9, -8, -3, -6, -10, -5,
    -6, -3, -3, -3, -3, -10, -3, -7,
    -4, -5, -12, -5, -5, -5, -5, -5,
    -5, -5, -2, -3, 5, -3, -5, -5,
    -2, -4, -7, -4, -7, -2, -5, -3,
    -2, -5, 6, 6, -38, -38, -20, -5,
    -5, -5, -5, -2, -20, -13, -13, -13,
    -13, -5, -3, -13, -13, -13, -9, -13,
    -13, -15, 5, -16, -6, -3, -6, -6,
    -13, -2, -3, -2, -36, -36, -12, -12,
    -19, -8, -8, -8, -8, -4, -14, -7,
    -7, -7, -7, 3, 3, -7, -7, -7,
    -3, -3, -18, -18, -6, -6, -10, -3,
    -3, -3, -3, -10, -5, -5, -5, -5,
    -5, -5, -5, -3, -6, -6, -6, -6,
    -3, -8, -6, -6, -10, -8, -8, -8,
    -8, -8, -8, -8, -3, -4, -9, -15,
    -10, -15, 4, 4, -40, -40, -15, -15,
    -23, -10, -10, -10, -10, -2, -28, -15,
    -15, -15, -4, -15, 2, -3, 2, -12,
    -12, -15, -10, -15, -12, -17, -2, -10,
    -10, -12, -10, -12, -5, -5, -5, -5,
    20, -15, -7, -19, -4, -4, -4, -10,
    -10, -13, -9, -5, -8, -15, -3, -5,
    -3, -5, -5, -11, -10, 5, 8, 8,
    4, 17, -4, -4, 10, 10, 7, 7,
    7, 4, 9, -6, -3, -3, -3, 3,
    -6, -2, -3, -3, -2, -3, -3, -3,
    -3, 3, 1, 1, 8, -9, -6, -4,
    -1, -4, 6, 6, 6, 8, -5, 14,
    11, -5, -6, -2, -5, -5, -5, -5,
    -5, -4, -5, -5, -5, -4, -5, -1,
    -3, -2, -3, -9, -6, -4, -1, -4,
    -9, -6, -4, -1, -4, -10, -10, -13,
    -9, -5, -8, -15, -3, -5, -3, -10,
    -10, -13, -9, -5, -8, -15, -3, -5,
    -3, -3, -3, 5, 6, 6, -24, -24,
    -11, -3, -3, -3, 2, -3, -3, 5,
    1, 1, -13, -7, -5, -3, -15, -3,
    -3, -3, 2, 1, -10, -3, -22, -22,
    -6, -6, -9, -3, -3, -3, -3, -3,
    -14, -14, -3, -6, -9, -6, -2, -5,
    -5, -5, -5, -5, -4, -5, -5, -5,
    -4, -22, -22, -6, -6, -9, -3, -3,
    -3, -3, -3, -13, 6, 18, 6
};

/*Collect the kern pair's data in one place*/
static const lv_font_fmt_txt_kern_pair_t kern_pairs =
{
    .glyph_ids = kern_pair_glyph_ids,
    .values = kern_pair_values,
    .pair_cnt = 495,
    .glyph_ids_size = 0
};

/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = &kern_pairs,
    .kern_scale = 16,
    .cmap_num = 2,
    .bpp = 1,
    .kern_classes = 0,
    .bitmap_format = 0,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
};



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t ui_font_Font1 = {
#else
lv_font_t ui_font_Font1 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 16,          /*The maximum line height required by the font*/
    .base_line = 3,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -1,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if UI_FONT_FONT1*/

