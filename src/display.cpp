#include "display.h"
#include "pins.h"
#include "config.h"
#include "settings.h"
#include <WiFi.h>
#include "wifi_full_icon.h"
#include "battery_level_64_64_28f.h"   // charging, 0-35%
#include "charged_battery_64_64_28f.h" // charging, 35-99%
#include "battery_icons.h"             // discharging: static glyphs, 100% down to 0%

static Arduino_DataBus *bus = nullptr;
static Arduino_GFX *panel = nullptr;
static Arduino_Canvas *canvas = nullptr;
Arduino_GFX *gfx = nullptr;

void displayInit() {
    // We must use Arduino_HWSPI to share the already-initialized SPI bus.
    // Arduino_ESP32SPI tries to initialize a new hardware SPI host, which causes a crash/boot loop.
    bus = new Arduino_HWSPI(BOARD_TFT_DC, BOARD_TFT_CS,
                            BOARD_SPI_SCK, BOARD_SPI_MOSI, BOARD_SPI_MISO);
                            
    // The ST7789 panel is natively 240x320 portrait. 
    // Rotation 1 sets it to 320x240 landscape. 
    panel = new Arduino_ST7789(bus, -1 /*RST tied to system*/, 1 /*rotation*/, true /*IPS*/);

    // BUGFIX (flicker): every screen in this firmware used to draw straight
    // to the physical SPI panel piece by piece - a fillRect to clear the
    // area, then text, then icons, each its own SPI transaction - which is
    // exactly what showed up as flicker: the screen visibly being erased
    // and rebuilt in front of you on every redraw. Wrapping the panel in an
    // off-screen RAM canvas means every one of those same draw calls
    // (nothing else in the codebase has to change - Arduino_Canvas supports
    // the whole Arduino_GFX drawing API) now writes into a framebuffer
    // instead. The physical screen is only ever updated by pushing that
    // already-finished framebuffer over in one burst via flush() (see
    // main.cpp's loop() and splash.cpp), so nothing partially drawn is ever
    // visible. The board's PSRAM (BOARD_HAS_PSRAM, opi_qio_opi in
    // platformio.ini) is exactly what a 320x240x16bpp (~150KB) framebuffer
    // needs.
    canvas = new Arduino_Canvas(SCREEN_W, SCREEN_H, panel);
    gfx = canvas;

    pinMode(BOARD_TFT_BACKLIGHT, OUTPUT);
#if defined(ESP_ARDUINO_VERSION_VAL) && ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3,0,0)
    ledcAttach(BOARD_TFT_BACKLIGHT, 5000, 8);
#else
    ledcSetup(0, 5000, 8);
    ledcAttachPin(BOARD_TFT_BACKLIGHT, 0);
#endif

    gfx->begin();
    gfx->fillScreen(0x0000); // raw RGB565 black - avoids depending on a BLACK macro that isn't reliably defined by this library/include order
    gfx->flush(); // push that initial black frame before anything else draws over it

    displaySetBacklight(gSettings.brightness);
}

void displaySetBacklight(uint8_t value) {
#if defined(ESP_ARDUINO_VERSION_VAL) && ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3,0,0)
    ledcWrite(BOARD_TFT_BACKLIGHT, value);
#else
    ledcWrite(0, value);
#endif
}

uint16_t *displayGetFramebuffer() {
    return canvas ? canvas->getFramebuffer() : nullptr;
}

// Nearest-neighbor scale of a 64x64 1bpp animation frame (same layout as
// menu.cpp/overlay.cpp's icons) into an arbitrary w x h box at (x, y). Only
// "on" bits are plotted (transparent background). Unlike menu.cpp's
// drawIconFrame/overlay.cpp's drawScaledFrame (both square-only), this scales
// width and height independently so a 64x64 source can land in the same
// 24x16 footprint as the static battery glyphs below without distorting -
// keeps the charging animations and the discharging statics the same size
// and position in the topbar.
static void drawBattFrameScaled(const uint8_t *frame, int x, int y, int w, int h, uint16_t color) {
    const int rowBytes = (ICON_SRC_SIZE + 7) / 8;
    for (int dy = 0; dy < h; dy++) {
        int sy = dy * ICON_SRC_SIZE / h;
        for (int dx = 0; dx < w; dx++) {
            int sx = dx * ICON_SRC_SIZE / w;
            uint8_t b = pgm_read_byte(&frame[sy * rowBytes + (sx / 8)]);
            if ((b >> (7 - (sx % 8))) & 0x01) {
                gfx->drawPixel(x + dx, y + dy, color);
            }
        }
    }
}

// Battery glyph shown in the topbar. Charging state gets one of two looping
// bitmap animations (fill level below/above 35%); discharging gets a plain
// static glyph picked from six pct bands. Both animation frame counters use
// the same millis()/42 % 28 cadence as the source lopaka export (~24fps) -
// independent of ICON_FRAME_MS, which paces the slower menu-row icons.
static void drawBatteryIcon(int x, int y, int pct, bool charging, uint16_t color) {
    if (charging) {
        if (pct < 35) {
            int frame = (millis() / 42) % 28;
            drawBattFrameScaled(battery_level_64_64_28f_frames[frame], x, y, BATT_ICON_W, BATT_ICON_H, color);
        } else {
            int frame = (millis() / 42) % 28;
            drawBattFrameScaled(charged_battery_64_64_28f_frames[frame], x, y, BATT_ICON_W, BATT_ICON_H, color);
        }
        return;
    }

    const unsigned char *bits;
    if (pct >= 90) bits = battery_full_bits;
    else if (pct >= 60) bits = battery_83_bits;
    else if (pct >= 50) bits = battery_67_bits;
    else if (pct >= 30) bits = battery_50_bits;
    else if (pct >= 10) bits = battery_33_bits;
    else bits = battery_10_bits;
    gfx->drawBitmap(x, y, bits, BATT_ICON_W, BATT_ICON_H, color);
}

void drawTopbar(int batteryPct, bool charging, bool sdOk) {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, 0, SCREEN_W, TOPBAR_HEIGHT, t.topbarBg);

    // Title. Left aligned
    gfx->setTextColor(t.accent);
    gfx->setCursor(6, 4);
    gfx->setTextSize(1);
    gfx->print(FW_NAME);

    // Version. Subtle, next to title
    gfx->setTextColor(t.dim);
    gfx->setCursor(6 + strlen(FW_NAME) * 6 + 4, 4);
    gfx->print(FW_VERSION);

    // Status icons (right aligned)
    int x = SCREEN_W - 6;

    // Battery percentage text
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", batteryPct);
    int battTextW = strlen(buf) * 6;
    x -= battTextW;
    gfx->setCursor(x, 4);
    uint16_t battColor = batteryPct < 20 ? t.bad : (batteryPct < 40 ? t.warn : t.ok);
    gfx->setTextColor(battColor);
    gfx->print(buf);

    if (charging) {
        gfx->setCursor(x - 10, 4);
        gfx->setTextColor(t.ok);
        gfx->print("+");
        x -= 10;
    }

    // Battery glyph: one of two looping charge animations while plugged in,
    // or a plain static glyph picked from six pct bands while discharging -
    // see drawBatteryIcon(). Replaces the old placeholder outline+fill rect.
    x -= 4;
    x -= BATT_ICON_W;
    int battY = (TOPBAR_HEIGHT - BATT_ICON_H) / 2;
    drawBatteryIcon(x, battY, batteryPct, charging, battColor);

    // Wi-Fi icon only, when actually connected to something. This used to
    // also draw small filled-circle status dots for BLE/LoRa/idle-Wi-Fi next
    // to it, packed close enough that the icon's right edge and the nearest
    // dot painted over the same pixels. Those dots weren't earning their
    // place (radio-on-but-idle isn't information worth a permanent topbar
    // light), so they're gone - just the Wi-Fi icon, and only when connected.
    if (WiFi.status() == WL_CONNECTED) {
        x -= 8;
        x -= 19;
        int iconY = (TOPBAR_HEIGHT - 16) / 2;
        gfx->drawBitmap(x, iconY, wifi_full_icon_bits, 19, 16, t.accent);
    }

    if (!sdOk) {
        gfx->setTextColor(t.warn);
        gfx->setCursor(x - 40, 4);
        gfx->print("NO SD");
    }

    gfx->drawFastHLine(0, TOPBAR_HEIGHT - 1, SCREEN_W, t.dim);
}
