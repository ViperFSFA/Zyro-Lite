#include "display.h"
#include "pins.h"
#include "config.h"
#include "settings.h"
#include <WiFi.h>
#include <SPI.h>
#include "wifi_full_icon.h"
#include <cstring>

class SharedDisplaySPI : public Arduino_HWSPI {
public:
    using Arduino_HWSPI::Arduino_HWSPI;
    // Use the core's burst pixel writer while preserving the existing shared-bus transaction.
    void writePixels(uint16_t *data,uint32_t count) override { SPI.writePixels(data,count*2); }
};
static Arduino_DataBus *bus = nullptr;
static Arduino_GFX *panel = nullptr;
static Arduino_Canvas *canvas = nullptr;
static uint16_t *presentedFrame = nullptr;
Arduino_GFX *gfx = nullptr;
static const char *screenContext = "HOME";
void displaySetContext(const char *label) { screenContext = label ? label : "HOME"; }

void displayInit() {
    // We must use Arduino_HWSPI to share the already-initialized SPI bus.
    // Arduino_ESP32SPI tries to initialize a new hardware SPI host, which causes a crash/boot loop.
    bus = new SharedDisplaySPI(BOARD_TFT_DC, BOARD_TFT_CS,
                            BOARD_SPI_SCK, BOARD_SPI_MOSI, BOARD_SPI_MISO);
                            
    panel = new Arduino_ST7789(bus, -1 /*RST tied to system*/, 1 /*rotation*/, true /*IPS*/);

    canvas = new Arduino_Canvas(SCREEN_W, SCREEN_H, panel);
    gfx = canvas;

    pinMode(BOARD_TFT_BACKLIGHT, OUTPUT);
    gfx->begin(80000000);
    gfx->fillScreen(0x0000); // raw RGB565 black - avoids depending on a BLACK macro that isn't reliably defined by this library/include order
    gfx->flush(); // push that initial black frame before anything else draws over it

    presentedFrame = (uint16_t *)ps_malloc(SCREEN_W * SCREEN_H * sizeof(uint16_t));
    if (presentedFrame) {
        memcpy(presentedFrame, canvas->getFramebuffer(), SCREEN_W * SCREEN_H * sizeof(uint16_t));
    }

    displaySetBacklight(gSettings.brightness);
}

void displaySetBacklight(uint8_t value) {
    // T-Deck uses a 16-step pulse-controlled backlight, not PWM.
    // Protocol matches LilyGO's UnitTest example.
    static uint8_t level = 0;
    uint8_t next = value ? max(1, (int(value)*16+254)/255) : 0;
    if (!next) { digitalWrite(BOARD_TFT_BACKLIGHT,LOW); delay(3); level=0; return; }
    if (!level) { digitalWrite(BOARD_TFT_BACKLIGHT,HIGH); delayMicroseconds(30); level=16; }
    int pulses=(level-next+16)%16;
    for(int i=0;i<pulses;i++) {
        digitalWrite(BOARD_TFT_BACKLIGHT,LOW); delayMicroseconds(1);
        digitalWrite(BOARD_TFT_BACKLIGHT,HIGH); delayMicroseconds(1);
    }
    level=next;
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

    // Keep one address window and transaction for the whole strided rectangle.
    Arduino_TFT *tft=static_cast<Arduino_TFT*>(panel);
    tft->startWrite();tft->writeAddrWindow(x,y,w,h);
    for (int row=0;row<h;row++) {
        uint16_t *src=fb+(y+row)*SCREEN_W+x;
        bus->writePixels(src,w);
        if(presentedFrame) memcpy(presentedFrame+(y+row)*SCREEN_W+x,src,w*2);
    }
    tft->endWrite();
}

bool displayFlushIfChanged() {
    if (!canvas) return false;
    uint16_t *fb = canvas->getFramebuffer();
    const size_t bytes = SCREEN_W * SCREEN_H * sizeof(uint16_t);
    if (presentedFrame && memcmp(fb, presentedFrame, bytes) == 0) return false;
    if(presentedFrame) return displayFlushDirtyRows();
    canvas->flush();
    return true;
}

// Compact battery meter. Percentage is a smoothed voltage-based estimate.
// Charging animation is reserved for a verified charger-status signal.
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
    gfx->setFont(NULL);
    gfx->setTextWrap(false);
    gfx->fillRect(0, 0, SCREEN_W, TOPBAR_HEIGHT, t.topbarBg);

    gfx->fillRoundRect(5,5,3,14,1,t.accent);
    gfx->setTextSize(1); gfx->setTextColor(t.fg);
    gfx->setCursor(13,3); gfx->print("ZYRO");
    gfx->setTextColor(t.dim); gfx->setCursor(13,14); gfx->print(FW_VERSION);
    gfx->drawFastVLine(49,6,12,t.dim);
    gfx->setTextColor(t.accent); gfx->setCursor(58,8); gfx->print(screenContext);

    // Status icons (right aligned)
    int x = SCREEN_W - 7;

    // Battery percentage text
    char buf[8];
    snprintf(buf, sizeof(buf), "~%d%%", batteryPct);
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
    if (gSettings.touchEnabled) gfx->fillRoundRect(149, 19, 22, 2, 1, t.accent);
}

void uiAppSurface() {
    const Theme &t=gSettings.theme();
    gfx->setFont(NULL); gfx->setTextSize(1); gfx->setTextWrap(false);
    gfx->fillRect(0,TOPBAR_HEIGHT,SCREEN_W,SCREEN_H-TOPBAR_HEIGHT,t.bg);
    gfx->fillRect(0,TOPBAR_HEIGHT,SCREEN_W,17,t.topbarBg);
    gfx->drawFastHLine(6,TOPBAR_HEIGHT+16,SCREEN_W-12,t.dim);
    gfx->drawFastHLine(6,TOPBAR_HEIGHT+16,32,t.accent);
}
void uiMetric(int x,int y,int w,const char *label,const String &value,uint16_t color) {
    const Theme &t=gSettings.theme();
    gfx->fillRoundRect(x,y,w,46,7,t.topbarBg);
    gfx->setFont(NULL); gfx->setTextSize(1); gfx->setTextColor(t.dim);
    gfx->setCursor(x+9,y+7); gfx->print(label);
    gfx->setTextColor(color); gfx->setTextSize(2);
    gfx->setCursor(x+9,y+23); gfx->print(value);
    gfx->setTextSize(1);
}

static uint16_t *viewportBacking=nullptr;
static int viewportTop=0,viewportBottom=SCREEN_H;
void uiBeginViewport(int top,int bottom) {
    viewportTop=constrain(top,0,SCREEN_H); viewportBottom=constrain(bottom,viewportTop,SCREEN_H);
    if(!viewportBacking) viewportBacking=(uint16_t*)ps_malloc(SCREEN_W*SCREEN_H*2);
    if(!viewportBacking) return;
    uint16_t *fb=displayGetFramebuffer();
    memcpy(viewportBacking,fb,viewportTop*SCREEN_W*2);
    memcpy(viewportBacking+viewportBottom*SCREEN_W,fb+viewportBottom*SCREEN_W,(SCREEN_H-viewportBottom)*SCREEN_W*2);
}
void uiEndViewport() {
    if(!viewportBacking) return;
    uint16_t *fb=displayGetFramebuffer();
    memcpy(fb,viewportBacking,viewportTop*SCREEN_W*2);
    memcpy(fb+viewportBottom*SCREEN_W,viewportBacking+viewportBottom*SCREEN_W,(SCREEN_H-viewportBottom)*SCREEN_W*2);
}

static uint32_t lastTransferUs=0;
uint32_t displayLastTransferUs() { return lastTransferUs; }
bool displayFlushDirtyRows(int top,int bottom) {
    uint32_t started=micros();uint16_t *fb=displayGetFramebuffer();if(!fb) return false;
    top=constrain(top,0,SCREEN_H);bottom=constrain(bottom,top,SCREEN_H);
    if(!presentedFrame) {displayFlushRegion(0,top,SCREEN_W,bottom-top);return true;}
    bool changed=false;
    // Sparse 16-row damage bands avoid sending the empty margins around text and cards.
    for(int band=top;band<bottom;band+=16) {
        int end=min(band+16,bottom),x0=SCREEN_W,x1=-1,y0=end,y1=-1;
        for(int y=band;y<end;y++) {
            uint16_t *a=fb+y*SCREEN_W,*b=presentedFrame+y*SCREEN_W;
            if(memcmp(a,b,SCREEN_W*2)==0) continue;
            int left=0,right=SCREEN_W-1;
            while(left<SCREEN_W && a[left]==b[left]) ++left;
            while(right>=left && a[right]==b[right]) --right;
            x0=min(x0,left);x1=max(x1,right);y0=min(y0,y);y1=y;
        }
        if(x1>=x0) {displayFlushRegion(x0,y0,x1-x0+1,y1-y0+1);changed=true;}
    }
    lastTransferUs=micros()-started;return changed;
}
