#pragma once
#include <Arduino.h>
#include "theme.h"

enum Handedness {
    HAND_RIGHT = 0, // Default: IJKL for right-handed navigation
    HAND_LEFT  = 1  // WASD for left-handed navigation
};

enum HighlightStyle {
    HIGHLIGHT_FILLED  = 0, // Default solid accent-colored pill behind the row
    HIGHLIGHT_OUTLINE = 1  // accent-colored outline only, row background unchanged
};

struct Settings {
    uint8_t themeId         = THEME_BLACK;
    uint8_t handedness      = HAND_RIGHT; // Default: Right-handed (IJKL)
    bool    trackballCursor = false;  // if true, trackball moves a free cursor instead of menu nav
    bool    soundEnabled    = false;    // Default OFF. Master switch: no clicks reach the speaker at all when off
    bool    batterySaver    = false;
    uint16_t screenTimeoutSec = 60;    // 0 = disabled, otherwise idle timeout in seconds
    uint8_t brightness      = 200;      // 0-255 backlight PWM
    bool    hapticClicks    = false;    // Default OFF. Extra gate on just the Nav click specifically (fires on every
                                         // up/down/left/right, far more often than OK/Back, so it stays opt-in to
                                         // avoid stutter from queuing I2S audio on rapid navigation). Sound must
                                         // also be on for Nav clicks to play. OK/Back only need Sound.
    uint8_t highlightStyle  = HIGHLIGHT_FILLED; // menu row selection look
    bool    keyboardBacklight = false;  // T-Deck keyboard's own backlight LEDs
    bool    enableBootLog   = false;    // if true, shows system log boot screen; else rotating dots splash

    const Theme &theme() const { return THEMES[themeId]; }
};

extern Settings gSettings;

// Settings now live in a plain-text /zyro.conf on the SD card (see
// settings.cpp) instead of NVS/Preferences. This gives us much more space,
// and it means anyone can pull the card, edit zyro.conf on a PC in a normal
// text editor, and put it back. Call settingsLoad() only after SD.begin()
// has already run in main.cpp. If there's no card, or no /zyro.conf yet,
// both functions fail soft: Load keeps in-RAM defaults, Save just no-ops
// rather than crashing.
void settingsLoad();
void settingsSave();
