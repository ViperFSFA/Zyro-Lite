#include "settings.h"
#include <SD.h>
#include "config.h"
#include <cstdlib>
#include <cerrno>
#include <climits>

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
    char *end = nullptr;
    errno = 0;
    long number = strtol(v.c_str(), &end, 10);
    if (errno || end == v.c_str() || *end || number < INT_MIN || number > INT_MAX) return def;
    return (int)number;
}

static bool toBool(const String &v, bool def) {
    if (v.length() == 0) return def;
    if (v == "1" || v.equalsIgnoreCase("true") || v.equalsIgnoreCase("on")) return true;
    if (v == "0" || v.equalsIgnoreCase("false") || v.equalsIgnoreCase("off")) return false;
    return def;
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

        if (key == "theme")          gSettings.themeId          = (uint8_t)constrain(toInt(value, THEME_BLACK), 0, THEME_COUNT-1);
        else if (key == "handed")    gSettings.handedness        = (uint8_t)toInt(value, HAND_RIGHT);
        else if (key == "tb_cursor") gSettings.trackballCursor   = toBool(value, false);
        else if (key == "nickname") {
            String safe; for(size_t i=0;i<value.length() && safe.length()<16;i++) { char c=value[i];if(isalnum((unsigned char)c)||c=='_'||c=='-') safe+=c; }
            if(safe.length()) safe.toCharArray(gSettings.consoleNickname,sizeof(gSettings.consoleNickname));
        }
        else if (key == "touch") gSettings.touchEnabled = toBool(value, true);
        else if (key == "enable_boot_log") gSettings.enableBootLog = toBool(value, false);
        else if (key == "sound")     gSettings.soundEnabled      = toBool(value, false);
        else if (key == "batt_saver")gSettings.batterySaver      = toBool(value, false);
        else if (key == "screen_timeout") gSettings.screenTimeoutSec = (uint16_t)constrain(toInt(value, 60), 0, 3600);
        else if (key == "bright")    gSettings.brightness        = (uint8_t)constrain(toInt(value, 200), 10, 255);
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
    f.printf("nickname=%s\n",gSettings.consoleNickname);
    f.printf("touch=%d\n", gSettings.touchEnabled ? 1 : 0);
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
