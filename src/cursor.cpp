#include "cursor.h"
#include <string.h>
#include "display.h"
#include "settings.h"
#include "config.h"

// [BEGIN lopaka generated]
static const unsigned char image_cursor_black_white_bits[] = {
    0x80,0x00,0xc0,0x00,0xe0,0x00,0xb0,0x00,0xb8,0x00,0x9c,0x00,0x9e,0x00,0x8f,0x00,
    0x8f,0x80,0x87,0xc0,0x8f,0xe0,0x90,0x00,0xa0,0x00,0xc0,0x00,0x80,0x00,0x00,0x00
};
// [END lopaka generated]
static int16_t cursorX = (SCREEN_W - CURSOR_W) / 2;
static int16_t cursorY = (SCREEN_H - CURSOR_H) / 2;
static int16_t drawnX = 0;
static int16_t drawnY = 0;
static bool backingValid = false;
static bool suppressed = false;
static uint16_t backing[CURSOR_H][CURSOR_W];

static bool cursorBitmapPixel(int x, int y) {
    const int rowBytes = (CURSOR_W + 7) / 8;
    uint8_t value = image_cursor_black_white_bits[y * rowBytes + (x / 8)];
    return ((value >> (7 - (x % 8))) & 0x01) != 0;
}

// A menu, app, or topbar may repaint beneath a stationary cursor between
// cursor ticks. Detect that before restoring the old backing; otherwise the
// stale rectangle would punch a small visual hole into the fresh screen.
static bool framebufferStillHasDrawnCursor() {
    if (!backingValid) return false;
    uint16_t *fb = displayGetFramebuffer();
    if (!fb) return false;
    for (int row = 0; row < CURSOR_H; row++) {
        for (int col = 0; col < CURSOR_W; col++) {
            uint16_t expected = cursorBitmapPixel(col, row) ? 0xFFFF : backing[row][col];
            if (fb[(drawnY + row) * SCREEN_W + drawnX + col] != expected) return false;
        }
    }
    return true;
}

void cursorInit() {
    cursorX = (SCREEN_W - CURSOR_W) / 2;
    cursorY = (SCREEN_H - CURSOR_H) / 2;
    backingValid = false;
    suppressed = false;
}

void cursorMove(int dx, int dy) {
    cursorX = (int16_t)constrain((int)cursorX + dx, 0, SCREEN_W - CURSOR_W);
    cursorY = (int16_t)constrain((int)cursorY + dy, 0, SCREEN_H - CURSOR_H);
}

void cursorSuppress(bool s) {
    suppressed = s;
}

void cursorInvalidate() {
    backingValid = false;
}

// Copies the CURSOR_W x CURSOR_H rectangle at (drawnX, drawnY) in the
// framebuffer back from `backing`, undoing the previous frame's sprite.
static void restoreUnder() {
    if (!backingValid) return;
    uint16_t *fb = displayGetFramebuffer();
    if (!fb) return;
    for (int row = 0; row < CURSOR_H; row++) {
        memcpy(&fb[(drawnY + row) * SCREEN_W + drawnX], backing[row], CURSOR_W * sizeof(uint16_t));
    }
    backingValid = false;
}

// Saves the CURSOR_W x CURSOR_H rectangle at the current (cursorX, cursorY)
// into `backing`, then draws the sprite on top of it.
static void drawOver() {
    uint16_t *fb = displayGetFramebuffer();
    if (!fb) return;
    for (int row = 0; row < CURSOR_H; row++) {
        memcpy(backing[row], &fb[(cursorY + row) * SCREEN_W + cursorX], CURSOR_W * sizeof(uint16_t));
    }
    backingValid = true;
    drawnX = cursorX;
    drawnY = cursorY;
    gfx->drawBitmap(cursorX, cursorY, image_cursor_black_white_bits, CURSOR_W, CURSOR_H, 0xFFFF);
}

void cursorTick() {
    bool shouldShow = gSettings.trackballCursor && !suppressed;
    if (!shouldShow) {
        restoreUnder();
        return;
    }
    
    if (backingValid && !framebufferStillHasDrawnCursor()) backingValid = false;
    restoreUnder();
    drawOver();
}
