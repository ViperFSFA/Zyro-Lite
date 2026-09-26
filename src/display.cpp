#include "display.h"
#include "pins.h"
#include "config.h"
#include "settings.h"
#include <WiFi.h>
#include "wifi_full_icon.h"
#include <cstring>

static Arduino_DataBus *bus = nullptr;
static Arduino_GFX *panel = nullptr;
static Arduino_Canvas *canvas = nullptr;
static uint16_t *presentedFrame = nullptr;
Arduino_GFX *gfx = nullptr;

void displayInit() {
    bus = new Arduino_HWSPI(BOARD_TFT_DC, BOARD_TFT_CS,
                            BOARD_SPI_SCK, BOARD_SPI_MOSI, BOARD_SPI_MISO);
                            
    panel = new Arduino_ST7789(bus, -1 /*RST tied to system*/, 1 /*rotation*/, true /*IPS*/);

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
    gfx->fillScreen(0x0000); 
    gfx->flush();

    presentedFrame = (uint16_t *)ps_malloc(SCREEN_W * SCREEN_H * sizeof(uint16_t));
    if (presentedFrame) {
        memcpy(presentedFrame, canvas->getFramebuffer(), SCREEN_W * SCREEN_H * sizeof(uint16_t));
    }

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

void displayFlushRegion(int x, int y, int w, int h) {
    if (!panel || !canvas || w <= 0 || h <= 0) return;
    x = constrain(x, 0, SCREEN_W - 1);
    y = constrain(y, 0, SCREEN_H - 1);
    w = min(w, SCREEN_W - x);
    h = min(h, SCREEN_H - y);

    uint16_t *fb = canvas->getFramebuffer();
    if (!fb) return;

    if (x == 0 && w == SCREEN_W) {
        panel->draw16bitRGBBitmap(0, y, fb + y * SCREEN_W, SCREEN_W, h);
        if (presentedFrame) {
            memcpy(presentedFrame + y * SCREEN_W, fb + y * SCREEN_W,
                   SCREEN_W * h * sizeof(uint16_t));
        }
        return;
    }

    for (int row = 0; row < h; row++) {
        uint16_t *src = fb + (y + row) * SCREEN_W + x;
        panel->draw16bitRGBBitmap(x, y + row, src, w, 1);
        if (presentedFrame) {
            memcpy(presentedFrame + (y + row) * SCREEN_W + x, src, w * sizeof(uint16_t));
        }
    }
}

bool displayFlushIfChanged() {
    if (!canvas) return false;
    uint16_t *fb = canvas->getFramebuffer();
    const size_t bytes = SCREEN_W * SCREEN_H * sizeof(uint16_t);
    if (presentedFrame && memcmp(fb, presentedFrame, bytes) == 0) return false;
    canvas->flush();
    if (presentedFrame) memcpy(presentedFrame, fb, bytes);
    return true;
}

// Compact battery icon
static const int TOP_BATT_W = 22;
static const int TOP_BATT_H = 12;

static void drawBatteryIcon(int x, int y, int pct, bool charging, uint16_t color, uint16_t bg) {
    const int bodyW = TOP_BATT_W - 3;
    gfx->drawRoundRect(x, y, bodyW, TOP_BATT_H, 3, color);
    gfx->fillRoundRect(x + bodyW, y + 3, 3, TOP_BATT_H - 6, 1, color);

    const int innerW = bodyW - 4;
    const int fillW = constrain(map(pct, 0, 100, 0, innerW), 0, innerW);
    if (fillW > 0) {
        gfx->fillRoundRect(x + 2, y + 2, fillW, TOP_BATT_H - 4, 2, color);
    }

    if (charging && fillW > 2) {
        int shimmer = (millis() / 85) % fillW;
        uint16_t shimmerColor = (shimmer & 1) ? bg : RGB565(255, 255, 255);
        gfx->drawFastVLine(x + 2 + shimmer, y + 3, TOP_BATT_H - 6, shimmerColor);
    }
}

void drawTopbar(int batteryPct, bool charging, bool sdOk) {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, 0, SCREEN_W, TOPBAR_HEIGHT, t.topbarBg);

    // Small brand marker and compact identity block.
    gfx->fillRoundRect(5, 7, 4, 10, 2, t.accent);
    gfx->setTextColor(t.accent);
    gfx->setCursor(13, 8);
    gfx->setTextSize(1);
    gfx->print(FW_NAME);

    // Version stays visible without competing with the device name.
    gfx->setTextColor(t.dim);
    gfx->setCursor(13 + strlen(FW_NAME) * 6 + 5, 8);
    gfx->print(FW_VERSION);

    // Status icons (right aligned)
    int x = SCREEN_W - 7;

    // Battery percentage text
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", batteryPct);
    int battTextW = strlen(buf) * 6;
    x -= battTextW;
    gfx->setCursor(x, 8);
    uint16_t battColor = charging ? t.ok : (batteryPct < 15 ? t.bad : (batteryPct < 30 ? t.warn : t.fg));
    gfx->setTextColor(battColor);
    gfx->print(buf);

    x -= 5 + TOP_BATT_W;
    int battY = (TOPBAR_HEIGHT - TOP_BATT_H) / 2;
    drawBatteryIcon(x, battY, batteryPct, charging, battColor, t.topbarBg);

    // Wi-Fi icon only, when actually connected to something.
    if (WiFi.status() == WL_CONNECTED) {
        x -= 7;
        x -= 19;
        int iconY = (TOPBAR_HEIGHT - 16) / 2;
        gfx->drawBitmap(x, iconY, wifi_full_icon_bits, 19, 16, t.ok);
    }

    if (!sdOk) {
        int badgeX = x - 31;
        gfx->fillRoundRect(badgeX, 4, 27, 16, 4, t.warn);
        gfx->setTextColor(t.topbarBg);
        gfx->setCursor(badgeX + 4, 8);
        gfx->print("SD!");
    }

    gfx->drawFastHLine(0, TOPBAR_HEIGHT - 1, SCREEN_W, t.dim);
}
