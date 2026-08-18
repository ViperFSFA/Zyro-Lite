#include "input.h"
#include <Wire.h>
#include "pins.h"
#include "config.h"
#include "settings.h"

static uint32_t lastKeyMs = 0;
static bool textEntryMode = false;

static bool sendKeyboardCommand(uint8_t cmd, uint8_t value);

void inputSetTextEntryMode(bool on) { textEntryMode = on; }

// The LilyGO T-Deck keyboard co-MCU I2C command set (from the official
// LilyGO keyboard firmware source):
//   0x01 = set backlight brightness (value 0x00=off, 0xFF=max)
//   0x02 = set backlight on/off     (value 0x00=off, non-zero=on)
// Both commands are accepted; command 0x02 is the simpler on/off toggle and
// more reliably supported across keyboard firmware revisions.
void keyboardSetBacklight(bool on) {
    uint8_t value = on ? (uint8_t)0xFF : (uint8_t)0x00;

    // Try command 0x02 (backlight enable/disable) first. Most revisions.
    bool ok = sendKeyboardCommand((uint8_t)0x02, value);
    // Also try command 0x01 (brightness) for newer firmware revisions.
    if (ok) sendKeyboardCommand((uint8_t)0x01, value);

    Serial.printf("[kbd] backlight %s -> %s\n", on ? "on" : "off", ok ? "OK" : "FAIL");
}

static bool sendKeyboardCommand(uint8_t cmd, uint8_t value) {
    // The keyboard MCU can be slow to wake up after a power cycle or deep
    // sleep. Retry with a back-off rather than giving up on the first NACK.
    for (int attempt = 0; attempt < 8; attempt++) {
        Wire.beginTransmission(KEYBOARD_I2C_ADDR);
        Wire.write(cmd);
        Wire.write(value);
        uint8_t err = Wire.endTransmission();
        if (err == 0) return true;
        Serial.printf("[kbd] I2C cmd 0x%02X attempt %d/8 err=%u\n", cmd, attempt + 1, (unsigned)err);
        delay(80);
        yield();
    }
    return false;
}

static bool keyboardRead(char &outCh) {
    Wire.requestFrom((uint8_t)KEYBOARD_I2C_ADDR, (uint8_t)1);
    if (!Wire.available()) return false;
    char c = Wire.read();
    if (c == 0x00 || c == (char)0xFF) return false;
    outCh = c;
    return true;
}

static InputEvent mapKeyToNav(char c) {
    if (textEntryMode) {
        switch (c) {
            case '\n': case '\r': return InputEvent::OK;
            case 0x08: case 0x7F:  return InputEvent::BACK;
            case 0x1B:             return InputEvent::BACK;
            case 0xB4:             return InputEvent::NAV_LEFT;
            case 0xB5:             return InputEvent::NAV_DOWN;
            case 0xB6:             return InputEvent::NAV_UP;
            case 0xB7:             return InputEvent::NAV_RIGHT;
            default: return InputEvent::NONE;
        }
    }

    switch (c) {
        case ' ':             return InputEvent::OK;
        case '\n': case '\r': return InputEvent::OK;
        case 0x08: case 0x7F: return InputEvent::BACK;
        case 0x1B:            return InputEvent::BACK;
        case 0xB4:            return InputEvent::NAV_LEFT;
        case 0xB5:            return InputEvent::NAV_DOWN;
        case 0xB6:            return InputEvent::NAV_UP;
        case 0xB7:            return InputEvent::NAV_RIGHT;
        default: break;
    }

    if (gSettings.handedness == HAND_RIGHT) {
        switch (c) {
            case 'i': case 'I': return InputEvent::NAV_UP;
            case 'k': case 'K': return InputEvent::NAV_DOWN;
            case 'j': case 'J': return InputEvent::NAV_LEFT;
            case 'l': case 'L': return InputEvent::NAV_RIGHT;
            default: break;
        }
    } else {
        switch (c) {
            case 'w': case 'W': return InputEvent::NAV_UP;
            case 's': case 'S': return InputEvent::NAV_DOWN;
            case 'a': case 'A': return InputEvent::NAV_LEFT;
            case 'd': case 'D': return InputEvent::NAV_RIGHT;
            default: break;
        }
    }

    return InputEvent::NONE;
}

static bool lastTbState[5] = {true, true, true, true, true};
static uint32_t lastClickMs = 0;
static uint32_t lastNavMs = 0;

void inputInit() {
    Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
    Wire.setTimeOut(20);

    // Increase I2C clock. The default 100kHz is fine but 400kHz (fast mode)
    // reduces blocking time during keyboard polls, which matters for the
    // overall loop() cadence.
    Wire.setClock(400000);

    pinMode(BOARD_KEYBOARD_INT, INPUT_PULLUP);

    pinMode(BOARD_TBOX_UP,    INPUT_PULLUP);
    pinMode(BOARD_TBOX_DOWN,  INPUT_PULLUP);
    pinMode(BOARD_TBOX_LEFT,  INPUT_PULLUP);
    pinMode(BOARD_TBOX_RIGHT, INPUT_PULLUP);
    pinMode(BOARD_TBOX_CLICK, INPUT_PULLUP);

    // The keyboard co-MCU needs time to finish its own boot sequence before
    // it will acknowledge I2C. Wait, then apply the backlight setting from
    // settings. Try twice: once just after the wait, and once more after a
    // short extra gap (catches the first acknowledgement window reliably).
    delay(600);
    // Wake-up ping: a zero-length transaction flushes any stale NAK state.
    Wire.beginTransmission(KEYBOARD_I2C_ADDR);
    Wire.endTransmission();
    delay(100);

    keyboardSetBacklight(gSettings.keyboardBacklight);
    delay(200);
    // Second attempt. Belt-and-suspenders for slow-waking units.
    keyboardSetBacklight(gSettings.keyboardBacklight);
}

InputResult inputPoll() {
    InputResult r;

    char c;
    if (millis() - lastKeyMs > 50) {
        lastKeyMs = millis();
        if (keyboardRead(c)) {
            InputEvent nav = mapKeyToNav(c);
            r.ch = c;
            if (nav != InputEvent::NONE) {
                r.type = nav;
            } else {
                r.type = InputEvent::CHAR;
            }
            return r;
        }
    }

    bool clickState = digitalRead(BOARD_TBOX_CLICK);
    if (clickState != lastTbState[4]) {
        lastTbState[4] = clickState;
        if (!clickState && (millis() - lastClickMs > 80)) {
            lastClickMs = millis();
            r.type = InputEvent::OK;
            return r;
        }
    }

    const uint8_t tbPins[4]       = { BOARD_TBOX_UP, BOARD_TBOX_RIGHT, BOARD_TBOX_DOWN, BOARD_TBOX_LEFT };
    const InputEvent tbEvents[4]  = { InputEvent::NAV_UP, InputEvent::NAV_RIGHT, InputEvent::NAV_DOWN, InputEvent::NAV_LEFT };
    const int8_t tbDx[4]          = { 0, 1, 0, -1 };
    const int8_t tbDy[4]          = { -1, 0, 1, 0 };

    for (int i = 0; i < 4; i++) {
        bool state = digitalRead(tbPins[i]);
        if (state != lastTbState[i]) {
            lastTbState[i] = state;
            if (!state && (millis() - lastNavMs > 30)) {
                lastNavMs = millis();
                if (gSettings.trackballCursor) {
                    r.type = InputEvent::CURSOR_MOVE;
                    r.dx = tbDx[i] * CURSOR_STEP_PX;
                    r.dy = tbDy[i] * CURSOR_STEP_PX;
                } else {
                    r.type = tbEvents[i];
                }
                return r;
            }
        }
    }

    r.type = InputEvent::NONE;
    return r;
}
