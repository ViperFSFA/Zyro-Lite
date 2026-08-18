#pragma once
#include <Arduino.h>

// 16-bit RGB565 colors
struct Theme {
    const char *name;
    uint16_t bg;
    uint16_t fg;         // primary text
    uint16_t dim;        // secondary text / icons
    uint16_t accent;     // highlight fill
    uint16_t accentFg;   // text on highlight
    uint16_t topbarBg;
    uint16_t ok;
    uint16_t warn;
    uint16_t bad;
};

// RGB565 helper
#ifndef RGB565
#define RGB565(r, g, b) ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
#endif

enum ThemeId {
    THEME_WHITE = 0,   // default
    THEME_BLACK,
    THEME_RED,
    THEME_GREEN,
    THEME_PURPLE,
    THEME_COUNT
};

static const Theme THEMES[THEME_COUNT] = {
    // WHITE
    { "White",
      RGB565(248,250,252), RGB565(15,23,42), RGB565(100,116,139),
      RGB565(37,99,235),   RGB565(255,255,255), RGB565(241,245,249),
      RGB565(16,185,129), RGB565(245,158,11), RGB565(239,68,68) },

    // BLACK
    { "Black",
      RGB565(11,11,14), RGB565(243,244,246), RGB565(107,114,128),
      RGB565(124,58,237), RGB565(255,255,255), RGB565(24,24,27),
      RGB565(16,185,129), RGB565(245,158,11), RGB565(239,68,68) },

    // RED (Dark Crimson)
    { "Red",
      RGB565(15,10,12), RGB565(254,242,242), RGB565(153,27,27),
      RGB565(225,29,72), RGB565(255,255,255), RGB565(30,12,16),
      RGB565(16,185,129), RGB565(245,158,11), RGB565(244,63,94) },

    // GREEN
    { "Green",
      RGB565(6,15,12), RGB565(236,253,245), RGB565(4,120,87),
      RGB565(16,185,129), RGB565(6,24,18), RGB565(12,28,22),
      RGB565(16,185,129), RGB565(245,158,11), RGB565(239,68,68) },

    // PURPLE
    { "Purple",
      RGB565(13,10,25), RGB565(250,245,255), RGB565(147,51,234),
      RGB565(168,85,247), RGB565(255,255,255), RGB565(24,18,40),
      RGB565(16,185,129), RGB565(245,158,11), RGB565(236,72,153) },
};
