#include "settings.h"
#include <SD.h>
#include "config.h"

Settings gSettings;

static const char *CONF_PATH = "/zyro.conf";

// Deliberately a plain "key=value" text file, one per line, rather than any binary format.
static bool parseLine(const String &line, String &key, String &value) {
    int eq = line.indexOf('=');
    if (eq < 0) return false;
    key = line.substring(0, eq);
    value = line.substring(eq + 1);
    key.trim();
    value.trim();
    return key.length() > 0;
}

static int toInt(const String &v, int def) {
    if (v.length() == 0) return def;
    return v.toInt();
}

static bool toBool(const String &v, bool def) {
    if (v.length() == 0) return def;
    return v == "1" || v.equalsIgnoreCase("true") || v.equalsIgnoreCase("on");
}

void settingsLoad() {
    File f = SD.open(CONF_PATH, FILE_READ);
    if (!f) {
        settingsSave();
        return;
    }

    while (f.available()) {
        String line = f.readStringUntil('\n');
        String key, value;
        if (!parseLine(line, key, value)) continue;

        if (key == "theme")          gSettings.themeId          = (uint8_t)toInt(value, THEME_BLACK);
        else if (key == "handed")    gSettings.handedness        = (uint8_t)toInt(value, HAND_RIGHT);
        else if (key == "tb_cursor") gSettings.trackballCursor   = toBool(value, false);
        else if (key == "sound")     gSettings.soundEnabled      = toBool(value, false);
        else if (key == "batt_saver")gSettings.batterySaver      = toBool(value, false);
        else if (key == "screen_timeout") gSettings.screenTimeoutSec = (uint16_t)toInt(value, 60);
        else if (key == "bright")    gSettings.brightness        = (uint8_t)toInt(value, 200);
        else if (key == "haptic")    gSettings.hapticClicks      = toBool(value, false);
        else if (key == "hl_style")  gSettings.highlightStyle    = (uint8_t)toInt(value, HIGHLIGHT_FILLED);
        else if (key == "kb_backlight") gSettings.keyboardBacklight = toBool(value, false);
    }
    f.close();

    if (gSettings.themeId >= THEME_COUNT) gSettings.themeId = THEME_BLACK;
    if (gSettings.handedness > HAND_LEFT) gSettings.handedness = HAND_RIGHT;
    if (gSettings.highlightStyle > HIGHLIGHT_OUTLINE) gSettings.highlightStyle = HIGHLIGHT_FILLED;
}

void settingsSave() {
    File f = SD.open(CONF_PATH, FILE_WRITE);
    if (!f) return; // no card / not mounted. fail, keep running on RAM defaults

    f.println("# Zyro-Lite configuration - safe to hand-edit, then reboot the device.");
    f.printf("theme=%u\n", gSettings.themeId);
    f.printf("handed=%u\n", gSettings.handedness);
    f.printf("tb_cursor=%d\n", gSettings.trackballCursor ? 1 : 0);
    f.printf("sound=%d\n", gSettings.soundEnabled ? 1 : 0);
    f.printf("batt_saver=%d\n", gSettings.batterySaver ? 1 : 0);
    f.printf("screen_timeout=%u\n", gSettings.screenTimeoutSec);
    f.printf("bright=%u\n", gSettings.brightness);
    f.printf("haptic=%d\n", gSettings.hapticClicks ? 1 : 0);
    f.printf("hl_style=%u\n", gSettings.highlightStyle);
    f.printf("kb_backlight=%d\n", gSettings.keyboardBacklight ? 1 : 0);
    f.printf("enable_boot_log=%d\n", gSettings.enableBootLog ? 1 : 0);
    f.close();
}
