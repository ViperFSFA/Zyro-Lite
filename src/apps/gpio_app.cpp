#include "app_api.h"
#include <SPI.h>
#include <RadioLib.h>
#include "pins.h"
#include "display.h"
#include "settings.h"
#include "config.h"
#include "audio.h"
#include "menu.h"

extern SPIClass *gSharedSPI;

// GPIO app: talks to whatever's plugged into the external connector.
//
// NRF24L01 / CC1101 submodes need real pin numbers in pins.h
// (GPIO_NRF24_*, GPIO_CC1101_*) before they'll do anything - see the big
// comment there. Until then they just report "Not configured" instead of
// guessing at wiring and potentially colliding with a pin already used
// elsewhere in the firmware.
//
// "Custom" pulls the pins listed in GPIO_CUSTOM_PINS (pins.h) HIGH, one at a
// time or together, for as long as you're in this app - onExit() always
// drives every one of them back to INPUT (effectively off) the instant you
// leave, whether you backed out normally or the app was closed some other
// way, so nothing is left energized after you've stopped looking at it.

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

// --- NRF24L01 ---
static nRF24 *nrf = nullptr;
static Module *nrfModule = nullptr;
static bool nrfConfigured = (GPIO_NRF24_CE_PIN >= 0 && GPIO_NRF24_CSN_PIN >= 0);
static bool nrfOk = false;

static void nrfSetup() {
    if (!nrfConfigured || nrfOk) return;
    nrfModule = new Module(GPIO_NRF24_CSN_PIN, RADIOLIB_NC, GPIO_NRF24_CE_PIN, RADIOLIB_NC, *gSharedSPI);
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
    gfx->print("NRF24L01");

    gfx->setTextColor(t.fg);
    gfx->setCursor(8, TOPBAR_HEIGHT + 26);
    if (!nrfConfigured) {
        gfx->setTextColor(t.warn);
        gfx->print("Not configured.");
        gfx->setCursor(8, TOPBAR_HEIGHT + 44);
        gfx->print("Set GPIO_NRF24_CE_PIN /");
        gfx->setCursor(8, TOPBAR_HEIGHT + 60);
        gfx->print("GPIO_NRF24_CSN_PIN in pins.h");
        return;
    }
    if (!nrfOk) {
        gfx->setTextColor(t.bad);
        gfx->print("Init failed - check wiring");
        return;
    }
    gfx->setTextColor(t.ok);
    gfx->print("Radio up, listening (2.4GHz)");

    if (nrf->available()) {
        String data;
        int state = nrf->readData(data);
        if (state == RADIOLIB_ERR_NONE) {
            gfx->setCursor(8, TOPBAR_HEIGHT + 46);
            gfx->setTextColor(t.fg);
            gfx->print("RX (" + String(data.length()) + "B): " + data.substring(0, 24));
        }
    }
}

// --- CC1101 ---
static CC1101 *cc = nullptr;
static Module *ccModule = nullptr;
static bool ccConfigured = (GPIO_CC1101_CS_PIN >= 0 && GPIO_CC1101_GDO0_PIN >= 0);
static bool ccOk = false;

static void ccSetup() {
    if (!ccConfigured || ccOk) return;
    ccModule = new Module(GPIO_CC1101_CS_PIN, GPIO_CC1101_GDO0_PIN,
                           RADIOLIB_NC, GPIO_CC1101_GDO2_PIN, *gSharedSPI);
    cc = new CC1101(ccModule);
    int state = cc->begin();
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
        gfx->print("Set GPIO_CC1101_CS_PIN /");
        gfx->setCursor(8, TOPBAR_HEIGHT + 60);
        gfx->print("GPIO_CC1101_GDO0_PIN in pins.h");
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

// --- Read GPIO ---
static void drawReadGpio() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);
    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print("Read GPIO");

    if (GPIO_CUSTOM_PIN_COUNT == 0) {
        gfx->setTextColor(t.warn);
        gfx->setCursor(8, TOPBAR_HEIGHT + 26);
        gfx->print("No pins listed yet.");
        gfx->setCursor(8, TOPBAR_HEIGHT + 42);
        gfx->print("Add them to GPIO_CUSTOM_PINS");
        gfx->setCursor(8, TOPBAR_HEIGHT + 58);
        gfx->print("in pins.h");
        return;
    }

    for (size_t i = 0; i < GPIO_CUSTOM_PIN_COUNT; i++) {
        int pin = GPIO_CUSTOM_PINS[i];
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

// --- Custom (pull selected pins HIGH) ---
static bool customPinOn[16] = {false}; // indexed same as GPIO_CUSTOM_PINS, capped at 16 entries
static int customSel = 0;

static void customApplyAll(bool allOff) {
    for (size_t i = 0; i < GPIO_CUSTOM_PIN_COUNT && i < 16; i++) {
        int pin = GPIO_CUSTOM_PINS[i];
        bool on = !allOff && customPinOn[i];
        pinMode(pin, OUTPUT);
        digitalWrite(pin, on ? HIGH : LOW);
        if (!on) pinMode(pin, INPUT); // fully release when off, not just driven low
    }
}

static void drawCustom() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);
    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print("Custom GPIO (OK=toggle HIGH)");

    if (GPIO_CUSTOM_PIN_COUNT == 0) {
        gfx->setTextColor(t.warn);
        gfx->setCursor(8, TOPBAR_HEIGHT + 26);
        gfx->print("No pins listed yet.");
        gfx->setCursor(8, TOPBAR_HEIGHT + 42);
        gfx->print("Add them to GPIO_CUSTOM_PINS");
        gfx->setCursor(8, TOPBAR_HEIGHT + 58);
        gfx->print("in pins.h");
        return;
    }

    for (size_t i = 0; i < GPIO_CUSTOM_PIN_COUNT; i++) {
        int pin = GPIO_CUSTOM_PINS[i];
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
            default: break; // MODE_CUSTOM only redraws on input, not on a timer
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
        if (currentMode == MODE_CUSTOM) customApplyAll(true); // release pins the moment you leave this screen
        currentMode = MODE_MENU;
        audioClickBack();
        if (subSubMenu) subSubMenu->draw();
        return;
    }

    if (currentMode == MODE_CUSTOM) {
        int n = (int)GPIO_CUSTOM_PIN_COUNT;
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
    // Hard guarantee: whatever Custom left driven HIGH turns off the instant
    // this app closes, regardless of how it closed.
    customApplyAll(true);

    if (subSubMenu) {
        delete subSubMenu;
        subSubMenu = nullptr;
    }
    if (nrfOk && nrf) nrf->sleep();
    if (ccOk && cc) cc->sleep();
}

static bool wantsExit() { return exitApp; }

}

AppModule gpioAppGet() {
    return { GpioApp::init, GpioApp::tick, GpioApp::handleInput, GpioApp::onExit, GpioApp::wantsExit };
}
