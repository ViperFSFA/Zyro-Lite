#include "app_api.h"
#include <SD.h>
#include "display.h"
#include "settings.h"
#include "config.h"
#include "audio.h"
#include "input.h"
#include "pins.h"

namespace SettingsApp {

enum MenuMode {
    MENU_ROOT = 0,
    MENU_PERSONALISATION,
    MENU_SCREEN,
    MENU_SOUND,
    MENU_KEYBOARD,
    MENU_POWER,
    MENU_FACTORY_RESET
};

static MenuMode mode = MENU_ROOT;
static int sel = 0;
static int scrollOffset = 0;
static bool exitRequested = false;
static bool confirmingReset = false;
static const int ROW_H = 28;
static const uint16_t kTimeoutValues[] = {0, 15, 30, 60, 120};

static int visibleRows() {
    int avail = SCREEN_H - TOPBAR_HEIGHT - 4;
    return avail / ROW_H;
}

static void ensureVisible(int rowCount) {
    int vis = visibleRows();
    if (sel < scrollOffset) {
        scrollOffset = sel;
    } else if (sel >= scrollOffset + vis) {
        scrollOffset = sel - vis + 1;
    }
    if (scrollOffset < 0) scrollOffset = 0;
    if (scrollOffset > rowCount - 1) scrollOffset = rowCount - 1;
}

static int rowCount() {
    switch (mode) {
        case MENU_PERSONALISATION: return 5;
        case MENU_SCREEN: return 3;
        case MENU_SOUND: return 3;
        case MENU_KEYBOARD: return 2;
        case MENU_POWER: return 4;
        case MENU_FACTORY_RESET: return 2;
        case MENU_ROOT:
        default: return 6;
    }
}

static bool isBackRow() {
    return sel == rowCount() - 1;
}

static String rowLabel(int index) {
    switch (mode) {
        case MENU_PERSONALISATION:
            switch (index) {
                case 0: return "Theme";
                case 1: return "Controls";
                case 2: return "Trackball";
                case 3: return "Highlight";
                default: return "Back";
            }
        case MENU_SCREEN:
            switch (index) {
                case 0: return "Brightness";
                case 1: return "Screen Timeout";
                default: return "Back";
            }
        case MENU_SOUND:
            switch (index) {
                case 0: return "Sound";
                case 1: return "Haptic Clicks";
                default: return "Back";
            }
        case MENU_KEYBOARD:
            switch (index) {
                case 0: return "Kbd Backlight";
                default: return "Back";
            }
        case MENU_POWER:
            switch (index) {
                case 0: return "Power Off";
                case 1: return "Restart";
                case 2: return "Battery Saver";
                default: return "Back";
            }
        case MENU_FACTORY_RESET:
            switch (index) {
                case 0: return "Factory Reset";
                default: return "Back";
            }
        case MENU_ROOT:
        default:
            switch (index) {
                case 0: return "Personalisation";
                case 1: return "Screen";
                case 2: return "Sound";
                case 3: return "Keyboard";
                case 4: return "Power Options";
                default: return "Factory Reset";
            }
    }
}

static String rowValue(int index) {
    switch (mode) {
        case MENU_PERSONALISATION:
            switch (index) {
                case 0: return gSettings.theme().name;
                case 1: return (gSettings.handedness == HAND_RIGHT) ? "Right" : "Left";
                case 2: return gSettings.trackballCursor ? "Cursor" : "Menu-nav";
                case 3: return (gSettings.highlightStyle == HIGHLIGHT_OUTLINE) ? "Outline" : "Filled";
                default: return "";
            }
        case MENU_SCREEN:
            switch (index) {
                case 0: return String((int)(gSettings.brightness * 100 / 255)) + "%";
                case 1: return (gSettings.screenTimeoutSec == 0) ? "Off" : String(gSettings.screenTimeoutSec) + "s";
                default: return "";
            }
        case MENU_SOUND:
            switch (index) {
                case 0: return gSettings.soundEnabled ? "On" : "Off";
                case 1: return gSettings.hapticClicks ? "On" : "Off";
                default: return "";
            }
        case MENU_KEYBOARD:
            return (index == 0) ? (gSettings.keyboardBacklight ? "On" : "Off") : "";
        case MENU_POWER:
            switch (index) {
                case 0: return "";
                case 1: return "";
                case 2: return gSettings.batterySaver ? "On" : "Off";
                default: return "";
            }
        case MENU_FACTORY_RESET:
            return (index == 0 && confirmingReset) ? "Press OK again" : "";
        case MENU_ROOT:
        default:
            return "";
    }
}

static void drawScrollbar(int rowCount) {
    const Theme &t = gSettings.theme();
    int vis = visibleRows();
    if (rowCount <= vis) return;

    int barAreaTop = TOPBAR_HEIGHT + 4;
    int barAreaH = SCREEN_H - barAreaTop - 4;
    int barX = SCREEN_W - 4;
    int barW = 3;

    gfx->fillRoundRect(barX, barAreaTop, barW, barAreaH, 1, t.dim);

    int thumbH = max(8, barAreaH * vis / rowCount);
    int thumbY = barAreaTop + (barAreaH - thumbH) * scrollOffset / (rowCount - vis);
    gfx->fillRoundRect(barX, thumbY, barW, thumbH, 1, t.accent);
}

static void redraw() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);
    gfx->setFont(UI_FONT);
    gfx->setTextSize(1);

    int rows = rowCount();
    int vis = visibleRows();

    for (int i = 0; i < rows; ++i) {
        if (i < scrollOffset || i >= scrollOffset + vis) continue;
        int screenRow = i - scrollOffset;
        int y = TOPBAR_HEIGHT + 4 + screenRow * ROW_H;
        bool hi = (i == sel);
        if (hi) {
            if (gSettings.highlightStyle == HIGHLIGHT_OUTLINE) {
                gfx->fillRoundRect(6, y, SCREEN_W - 12, 24, 6, t.bg);
                gfx->drawRoundRect(6, y, SCREEN_W - 12, 24, 6, t.accent);
                gfx->setTextColor(t.accent);
            } else {
                gfx->fillRoundRect(6, y, SCREEN_W - 12, 24, 6, t.accent);
                gfx->setTextColor(t.accentFg);
            }
        } else {
            gfx->setTextColor(t.fg);
        }

        int16_t bx, by; uint16_t bw, bh;
        String label = rowLabel(i);
        String value = rowValue(i);
        gfx->getTextBounds(label.c_str(), 0, 0, &bx, &by, &bw, &bh);
        gfx->setCursor(14, uiFontTextBaselineY(y + 2, 24, label.c_str()));
        gfx->print(label);
        gfx->getTextBounds(value.c_str(), 0, 0, &bx, &by, &bw, &bh);
        gfx->setCursor(SCREEN_W - 14 - (int)bw, uiFontTextBaselineY(y + 2, 24, value.c_str()));
        gfx->print(value);
    }

    gfx->setFont(NULL);
    drawScrollbar(rows);
}

static void init() {
    mode = MENU_ROOT;
    sel = 0;
    scrollOffset = 0;
    exitRequested = false;
    confirmingReset = false;
    redraw();
}

static void tick() {}

static void adjust(int dir) {
    switch (mode) {
        case MENU_PERSONALISATION:
            switch (sel) {
                case 0:
                    gSettings.themeId = (gSettings.themeId + THEME_COUNT + dir) % THEME_COUNT;
                    break;
                case 1:
                    gSettings.handedness = (gSettings.handedness == HAND_RIGHT) ? HAND_LEFT : HAND_RIGHT;
                    break;
                case 2:
                    gSettings.trackballCursor = !gSettings.trackballCursor;
                    break;
                case 3:
                    gSettings.highlightStyle = (gSettings.highlightStyle == HIGHLIGHT_FILLED) ? HIGHLIGHT_OUTLINE : HIGHLIGHT_FILLED;
                    break;
                default:
                    break;
            }
            break;
        case MENU_SCREEN:
            if (sel == 0) {
                int next = (int)gSettings.brightness + dir * 15;
                gSettings.brightness = (uint8_t)constrain(next, 10, 255);
                displaySetBacklight(gSettings.brightness);
            } else if (sel == 1) {
                const uint16_t *values = kTimeoutValues;
                int count = (int)(sizeof(kTimeoutValues) / sizeof(kTimeoutValues[0]));
                int current = 0;
                for (int i = 0; i < count; ++i) {
                    if (kTimeoutValues[i] == gSettings.screenTimeoutSec) { current = i; break; }
                }
                int next = current + dir;
                if (next < 0) next = 0;
                if (next >= count) next = count - 1;
                gSettings.screenTimeoutSec = kTimeoutValues[next];
            }
            break;
        case MENU_SOUND:
            if (sel == 0) {
                gSettings.soundEnabled = !gSettings.soundEnabled;
            } else if (sel == 1) {
                gSettings.hapticClicks = !gSettings.hapticClicks;
            }
            break;
        case MENU_KEYBOARD:
            if (sel == 0) {
                gSettings.keyboardBacklight = !gSettings.keyboardBacklight;
                keyboardSetBacklight(gSettings.keyboardBacklight);
            }
            break;
        case MENU_POWER:
            if (sel == 2) {
                gSettings.batterySaver = !gSettings.batterySaver;
            }
            break;
        default:
            break;
    }

    settingsSave();
    redraw();
}

static void showActionScreen(const char *label) {
    const Theme &t = gSettings.theme();
    gfx->fillScreen(t.bg);
    gfx->setFont(UI_FONT);
    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(20, uiFontTextBaselineY(SCREEN_H / 2 - 18, 24, label));
    gfx->print(label);
    gfx->flush();
}

static void doPowerOff() {
    showActionScreen("Powering off...");
    delay(400);
    keyboardSetBacklight(false);
    displaySetBacklight(0);
    gfx->fillScreen(gSettings.theme().bg);
    gfx->flush();
    delay(200);
    digitalWrite(BOARD_POWERON, LOW);
    while (true) {
        delay(1000);
    }
}

static void doReboot() {
    showActionScreen("Restarting...");
    delay(400);
    keyboardSetBacklight(false);
    displaySetBacklight(0);
    gfx->fillScreen(gSettings.theme().bg);
    gfx->flush();
    delay(200);
    ESP.restart();
}

static void doFactoryReset() {
    gSettings = Settings();
    if (SD.exists("/zyro.conf")) {
        SD.remove("/zyro.conf");
    }
    settingsSave();
    displaySetBacklight(gSettings.brightness);
    keyboardSetBacklight(gSettings.keyboardBacklight);
    mode = MENU_ROOT;
    sel = 0;
    scrollOffset = 0;
    confirmingReset = false;
    redraw();
}

static void enterCurrentCategory() {
    switch (mode) {
        case MENU_ROOT:
            switch (sel) {
                case 0: mode = MENU_PERSONALISATION; break;
                case 1: mode = MENU_SCREEN; break;
                case 2: mode = MENU_SOUND; break;
                case 3: mode = MENU_KEYBOARD; break;
                case 4: mode = MENU_POWER; break;
                case 5: mode = MENU_FACTORY_RESET; break;
                default: break;
            }
            break;
        default:
            break;
    }
    sel = 0;
    scrollOffset = 0;
    confirmingReset = false;
    redraw();
}

static void handleInput(const InputResult &in) {
    switch (in.type) {
        case InputEvent::NAV_UP:
            sel = (sel - 1 + rowCount()) % rowCount();
            ensureVisible(rowCount());
            audioClickNav();
            redraw();
            break;
        case InputEvent::NAV_DOWN:
            sel = (sel + 1) % rowCount();
            ensureVisible(rowCount());
            audioClickNav();
            redraw();
            break;
        case InputEvent::NAV_LEFT:
            if (mode != MENU_ROOT) {
                if (isBackRow()) {
                    mode = MENU_ROOT;
                    sel = 0;
                    scrollOffset = 0;
                    confirmingReset = false;
                    redraw();
                } else {
                    adjust(-1);
                }
            }
            break;
        case InputEvent::NAV_RIGHT:
            if (mode != MENU_ROOT) {
                if (isBackRow()) {
                    mode = MENU_ROOT;
                    sel = 0;
                    scrollOffset = 0;
                    confirmingReset = false;
                    redraw();
                } else {
                    adjust(1);
                }
            }
            break;
        case InputEvent::OK:
            if (mode == MENU_ROOT) {
                enterCurrentCategory();
            } else if (isBackRow()) {
                mode = MENU_ROOT;
                sel = 0;
                scrollOffset = 0;
                confirmingReset = false;
                redraw();
            } else if (mode == MENU_FACTORY_RESET) {
                if (confirmingReset) {
                    confirmingReset = false;
                    doFactoryReset();
                } else {
                    confirmingReset = true;
                    redraw();
                }
            } else if (mode == MENU_POWER) {
                if (sel == 0) {
                    doPowerOff();
                } else if (sel == 1) {
                    doReboot();
                } else {
                    adjust(1);
                }
            } else {
                adjust(1);
            }
            audioClickOk();
            break;
        case InputEvent::BACK:
            if (mode == MENU_ROOT) {
                exitRequested = true;
            } else {
                mode = MENU_ROOT;
                sel = 0;
                scrollOffset = 0;
                confirmingReset = false;
                redraw();
            }
            audioClickBack();
            break;
        default:
            break;
    }
}

static void onExit() {}
static bool wantsExit() { return exitRequested; }

}

AppModule settingsAppGet() {
    return { SettingsApp::init, SettingsApp::tick, SettingsApp::handleInput,
             SettingsApp::onExit, SettingsApp::wantsExit };
}
