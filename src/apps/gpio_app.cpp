#include "app_api.h"
#include <SPI.h>
#include <SD.h>
#include <RadioLib.h>
#include <vector>
#include "pins.h"
#include "display.h"
#include "settings.h"
#include "config.h"
#include "audio.h"
#include "menu.h"

extern SPIClass *gSharedSPI;

// GPIO app: talks to external connector pins.
// Pins are configured via /gpio_pins.conf on the SD card (auto-generated if missing).

namespace GpioApp {

enum Mode {
    MODE_MENU = 0,
    MODE_NRF24,
    MODE_CC1101,
    MODE_READ,
    MODE_CUSTOM
};

static Mode currentMode = MODE_MENU;
static Menu *subSubMenu = nullptr;
static bool exitApp = false;
static uint32_t lastDrawMs = 0;

// Dynamic pin config loaded from /gpio_pins.conf on SD card
static std::vector<int> dynamicCustomPins = { 16, 43, 44 }; // Default T-Deck pins (16=Touch INT, 43=GPS TX, 44=GPS RX)
static int dynamicNrfCe = GPIO_NRF24_CE_PIN;
static int dynamicNrfCsn = GPIO_NRF24_CSN_PIN;
static int dynamicCcCs = GPIO_CC1101_CS_PIN;
static int dynamicCcGdo0 = GPIO_CC1101_GDO0_PIN;
static int dynamicCcGdo2 = GPIO_CC1101_GDO2_PIN;

// --- NRF24L01 ---
static nRF24 *nrf = nullptr;
static Module *nrfModule = nullptr;
static bool nrfConfigured = false;
static bool nrfOk = false;

// --- CC1101 ---
static CC1101 *cc = nullptr;
static Module *ccModule = nullptr;
static bool ccConfigured = false;
static bool ccOk = false;

// --- Custom Pin Control ---
static bool customPinOn[16] = {false};
static int customSel = 0;

static void loadGpioPinsConfig() {
    dynamicCustomPins = { 16, 43, 44 };
    dynamicNrfCe = GPIO_NRF24_CE_PIN;
    dynamicNrfCsn = GPIO_NRF24_CSN_PIN;
    dynamicCcCs = GPIO_CC1101_CS_PIN;
    dynamicCcGdo0 = GPIO_CC1101_GDO0_PIN;
    dynamicCcGdo2 = GPIO_CC1101_GDO2_PIN;

    if (!SD.exists("/gpio_pins.conf")) {
        File f = SD.open("/gpio_pins.conf", FILE_WRITE);
        if (f) {
            f.println("# Zyro-Lite GPIO Pin Map");
            f.println("# Default T-Deck pins: 16 (Touch INT), 43 (GPS TX), 44 (GPS RX)");
            f.println("CUSTOM_PINS=16,43,44");
            f.println("NRF24_CE=-1");
            f.println("NRF24_CSN=-1");
            f.println("CC1101_CS=-1");
            f.println("CC1101_GDO0=-1");
            f.println("CC1101_GDO2=-1");
            f.close();
        }
    } else {
        File f = SD.open("/gpio_pins.conf", FILE_READ);
        if (f) {
            dynamicCustomPins.clear();
            while (f.available()) {
                String line = f.readStringUntil('\n');
                line.trim();
                if (line.length() == 0 || line.startsWith("#")) continue;

                int eq = line.indexOf('=');
                if (eq < 0) continue;
                String key = line.substring(0, eq);
                String val = line.substring(eq + 1);
                key.trim(); val.trim();

                if (key == "CUSTOM_PINS") {
                    int start = 0;
                    while (start < (int)val.length()) {
                        int comma = val.indexOf(',', start);
                        if (comma < 0) comma = (int)val.length();
                        String pStr = val.substring(start, comma);
                        pStr.trim();
                        if (pStr.length() > 0) {
                            int pin = pStr.toInt();
                            if (pin >= 0) dynamicCustomPins.push_back(pin);
                        }
                        start = comma + 1;
                    }
                } else if (key == "NRF24_CE") dynamicNrfCe = val.toInt();
                else if (key == "NRF24_CSN") dynamicNrfCsn = val.toInt();
                else if (key == "CC1101_CS") dynamicCcCs = val.toInt();
                else if (key == "CC1101_GDO0") dynamicCcGdo0 = val.toInt();
                else if (key == "CC1101_GDO2") dynamicCcGdo2 = val.toInt();
            }
            f.close();
        }
    }

    if (dynamicCustomPins.empty()) {
        dynamicCustomPins = { 16, 43, 44 };
    }
    nrfConfigured = (dynamicNrfCe >= 0 && dynamicNrfCsn >= 0);
    ccConfigured = (dynamicCcCs >= 0 && dynamicCcGdo0 >= 0);
}

static void nrfSetup() {
    if (!nrfConfigured || nrfOk) return;
    nrfModule = new Module(dynamicNrfCsn, RADIOLIB_NC, dynamicNrfCe, RADIOLIB_NC, *gSharedSPI);
    nrf = new nRF24(nrfModule);
    int state = nrf->begin();
    nrfOk = (state == RADIOLIB_ERR_NONE);
    if (nrfOk) nrf->startReceive();
}

static void drawNrf24() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);
    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print("NRF24L01 Radio");

    gfx->setTextColor(t.fg);
    gfx->setCursor(8, TOPBAR_HEIGHT + 26);
    if (!nrfConfigured) {
        gfx->setTextColor(t.warn);
        gfx->print("Not configured.");
        gfx->setCursor(8, TOPBAR_HEIGHT + 44);
        gfx->print("Set NRF24_CE / NRF24_CSN in");
        gfx->setCursor(8, TOPBAR_HEIGHT + 60);
        gfx->print("gpio_pins.conf on SD card");
        return;
    }
    if (!nrfOk) {
        gfx->setTextColor(t.bad);
        gfx->print("Init failed - check 3.3V/SPI wiring");
        return;
    }
    gfx->setTextColor(t.ok);
    gfx->print("Radio up, listening (2.4GHz)");

    if (nrf && nrf->available()) {
        String data;
        int state = nrf->readData(data);
        if (state == RADIOLIB_ERR_NONE) {
            gfx->setCursor(8, TOPBAR_HEIGHT + 46);
            gfx->setTextColor(t.fg);
            gfx->print("RX (" + String(data.length()) + "B): " + data.substring(0, 24));
        }
    }
}

static void ccSetup() {
    if (!ccConfigured || ccOk) return;
    ccModule = new Module(dynamicCcCs, dynamicCcGdo0, RADIOLIB_NC, dynamicCcGdo2, *gSharedSPI);
    cc = new CC1101(ccModule);
    int state = cc->begin(433.92);
    ccOk = (state == RADIOLIB_ERR_NONE);
}

static void drawCc1101() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);
    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print("CC1101 Sub-GHz");

    gfx->setTextColor(t.fg);
    gfx->setCursor(8, TOPBAR_HEIGHT + 26);
    if (!ccConfigured) {
        gfx->setTextColor(t.warn);
        gfx->print("Not configured.");
        gfx->setCursor(8, TOPBAR_HEIGHT + 44);
        gfx->print("Set CC1101_CS / CC1101_GDO0 in");
        gfx->setCursor(8, TOPBAR_HEIGHT + 60);
        gfx->print("gpio_pins.conf on SD card");
        return;
    }
    if (!ccOk) {
        gfx->setTextColor(t.bad);
        gfx->print("Init failed - check wiring");
        return;
    }
    gfx->setTextColor(t.ok);
    gfx->print("Radio up (433.92 MHz default)");
    gfx->setCursor(8, TOPBAR_HEIGHT + 46);
    gfx->setTextColor(t.dim);
    gfx->print("RSSI: " + String(cc->getRSSI(), 1) + " dBm");
}

static void drawReadGpio() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);
    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print("Read GPIO");

    size_t n = dynamicCustomPins.size();
    if (n == 0) {
        gfx->setTextColor(t.warn);
        gfx->setCursor(8, TOPBAR_HEIGHT + 26);
        gfx->print("No pins defined in gpio_pins.conf");
        return;
    }

    for (size_t i = 0; i < n && i < 16; i++) {
        int pin = dynamicCustomPins[i];
        pinMode(pin, INPUT);
        bool state = digitalRead(pin);
        int y = TOPBAR_HEIGHT + 26 + i * 20;
        gfx->setTextColor(t.fg);
        gfx->setCursor(8, y);
        gfx->print("GPIO " + String(pin) + ": ");
        gfx->setTextColor(state ? t.ok : t.dim);
        gfx->print(state ? "HIGH" : "LOW");
    }
}

static void customApplyAll(bool allOff) {
    for (size_t i = 0; i < dynamicCustomPins.size() && i < 16; i++) {
        int pin = dynamicCustomPins[i];
        bool on = !allOff && customPinOn[i];
        pinMode(pin, OUTPUT);
        digitalWrite(pin, on ? HIGH : LOW);
        if (!on) pinMode(pin, INPUT);
    }
}

static void drawCustom() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);
    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print("Custom GPIO (OK=toggle HIGH)");

    size_t n = dynamicCustomPins.size();
    if (n == 0) {
        gfx->setTextColor(t.warn);
        gfx->setCursor(8, TOPBAR_HEIGHT + 26);
        gfx->print("No pins listed in gpio_pins.conf");
        return;
    }

    for (size_t i = 0; i < n && i < 16; i++) {
        int pin = dynamicCustomPins[i];
        int y = TOPBAR_HEIGHT + 26 + i * 22;
        bool hi = ((int)i == customSel);
        if (hi) gfx->fillRect(4, y - 2, SCREEN_W - 8, 18, t.accent);
        gfx->setTextColor(hi ? t.accentFg : t.fg);
        gfx->setCursor(8, y);
        gfx->print("GPIO " + String(pin) + ": " + (customPinOn[i] ? "HIGH (driving)" : "off"));
    }

    gfx->setTextColor(t.warn);
    gfx->setCursor(8, SCREEN_H - 16);
    gfx->print("All pins snap back off when you exit this app.");
}

static void init() {
    exitApp = false;
    currentMode = MODE_MENU;
    loadGpioPinsConfig();

    if (subSubMenu) delete subSubMenu;
    std::vector<MenuItem> items = {
        { "NRF24L01",   ">", [](){ currentMode = MODE_NRF24;  nrfSetup(); drawNrf24(); } },
        { "CC1101",     ">", [](){ currentMode = MODE_CC1101; ccSetup();  drawCc1101(); } },
        { "Read GPIO",  ">", [](){ currentMode = MODE_READ;   drawReadGpio(); } },
        { "Custom",     ">", [](){ currentMode = MODE_CUSTOM; customSel = 0; drawCustom(); } },
    };
    subSubMenu = new Menu("GPIO Tools", items);
    subSubMenu->draw();
}

static void tick() {
    if (currentMode == MODE_MENU && subSubMenu) {
        subSubMenu->tick();
        return;
    }

    if (millis() - lastDrawMs > 400) {
        lastDrawMs = millis();
        switch (currentMode) {
            case MODE_NRF24:  drawNrf24();  break;
            case MODE_CC1101: drawCc1101(); break;
            case MODE_READ:   drawReadGpio(); break;
            default: break;
        }
    }
}

static void handleInput(const InputResult &in) {
    if (currentMode == MODE_MENU) {
        if (in.type == InputEvent::BACK) {
            exitApp = true;
            audioClickBack();
            return;
        }
        if (subSubMenu) subSubMenu->handleInput(in);
        return;
    }

    if (in.type == InputEvent::BACK) {
        if (currentMode == MODE_CUSTOM) customApplyAll(true);
        currentMode = MODE_MENU;
        audioClickBack();
        if (subSubMenu) subSubMenu->draw();
        return;
    }

    if (currentMode == MODE_CUSTOM) {
        int n = (int)dynamicCustomPins.size();
        if (n == 0) return;
        if (in.type == InputEvent::NAV_UP) {
            customSel = (customSel - 1 + n) % n;
            audioClickNav(); drawCustom();
        } else if (in.type == InputEvent::NAV_DOWN) {
            customSel = (customSel + 1) % n;
            audioClickNav(); drawCustom();
        } else if (in.type == InputEvent::OK) {
            if (customSel < 16) customPinOn[customSel] = !customPinOn[customSel];
            customApplyAll(false);
            audioClickOk(); drawCustom();
        }
    }
}

static void onExit() {
    customApplyAll(true);

    if (subSubMenu) {
        delete subSubMenu;
        subSubMenu = nullptr;
    }
    if (nrfOk && nrf) nrf->sleep();
    if (ccOk && cc) cc->sleep();
}

static bool wantsExit() { return exitApp; }

} // namespace GpioApp

AppModule gpioAppGet() {
    return { GpioApp::init, GpioApp::tick, GpioApp::handleInput, GpioApp::onExit, GpioApp::wantsExit };
}
