#pragma once
#include <Arduino_GFX_Library.h>
#include "fonts/JetBrainsMono12pt7b.h"

extern Arduino_GFX *gfx;

// Readable UI font for single-line, generously-spaced content: root menu
// rows, every app's own sub-menu, Settings rows, and alert popups. NOT used
// for the dense multi-line diagnostic screens (Wi-Fi/BLE scan results,
// spectrum analyzer, GPS/Ethernet readouts, packet monitor) - those pack
// far more characters per row than this monospace font's glyph width
// allows, and would overlap/overflow the 320x240 panel. Those screens
// intentionally keep the compact built-in font.
// Bumped 8pt -> 12pt: 8pt was reading slightly too small on the physical
// panel. 12pt still leaves comfortable margin on every existing menu label
// (longest current label is ~19 chars, well under what fits at this size
// within SCREEN_W with the row's icon + padding).
#define UI_FONT (&JetBrainsMono12pt7b)
// The UI font draws from its text baseline, so use the actual glyph bounds to
// center text vertically inside a row/card instead of relying on a single
// hard-coded offset.
#define UI_FONT_BASELINE 8

inline int uiFontTextBaselineY(int top, int height, const char *text) {
    if (!gfx) return top;
    if (!text) text = "";
    int16_t bx, by; uint16_t bw, bh;
    gfx->getTextBounds(text, 0, 0, &bx, &by, &bw, &bh);
    return top + (height - (int)bh) / 2 - (int)by;
}

void displayInit();
void displaySetBacklight(uint8_t value); // 0-255
void drawTopbar(int batteryPct, bool charging, bool sdOk);

// Raw access to the canvas's RGB565 framebuffer (row-major, stride ==
// SCREEN_W, no padding - Arduino_Canvas allocates exactly w*h uint16_t's).
// Used by cursor.cpp to save/restore the small rectangle of pixels under the
// on-screen cursor sprite, since this display has no other way to "un-draw"
// something without knowing what was underneath it. Returns nullptr if
// called before displayInit().
uint16_t *displayGetFramebuffer();
