#pragma once
#include <Arduino.h>

enum class InputEvent {
    NONE = 0,
    NAV_UP,
    NAV_DOWN,
    NAV_LEFT,
    NAV_RIGHT,
    OK,          // trackball click OR SPACE
    BACK,        // ESC / Backspace
    CHAR,        // printable character available in .ch
    CURSOR_MOVE  // only emitted when trackballCursor mode is on: .dx/.dy filled
};

struct InputResult {
    InputEvent type = InputEvent::NONE;
    char ch = 0;
    int dx = 0;
    int dy = 0;
};

void inputInit();
InputResult inputPoll();

// While on, the keyboard driver stops treating letters (IJKL/WASD) and space
// as navigation/OK shortcuts and instead passes them through as plain CHAR
// events. needed for anything that takes free text (Wi-Fi passwords, etc.)
// where those same keys are legitimate characters to type. Enter still
// submits (OK) and Backspace/Esc still cancel/erase (BACK). turn it back off
// once the text field is done.
void inputSetTextEntryMode(bool on);

// Toggles the T-Deck keyboard's own backlight LEDs via its I2C controller.
// Confirmed against LilyGO's own keyboard firmware: register 0x01
// (LILYGO_KB_BRIGHTNESS_CMD) takes a brightness value 0-255, 0 = off.
void keyboardSetBacklight(bool on);
