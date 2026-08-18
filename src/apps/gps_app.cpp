#include "app_api.h"
#include <TinyGPSPlus.h>
#include <SD.h>
#include "pins.h"
#include "display.h"
#include "settings.h"
#include "config.h"
#include "audio.h"
#include "menu.h"

namespace GpsApp {

enum Mode {
    MODE_MENU = 0,
    MODE_FIX,
    MODE_TRACKER,
    MODE_COMPASS,
    MODE_LOGGER
};

static Mode currentMode = MODE_MENU;
static Menu *subSubMenu = nullptr;
static bool exitApp = false;

static TinyGPSPlus gps;
static bool serialOk = false;
static uint32_t lastDrawMs = 0;

// Coordinate logger state
static bool logActive = false;
static uint32_t logCount = 0;
static String logPath;
static uint32_t lastLogMs = 0;
static const uint32_t LOG_INTERVAL_MS = 5000; // log a point every 5 seconds

// Speed / heading tracker
static float maxSpeedKmh = 0.0f;
static float totalDistKm = 0.0f;
static double lastLat = 0.0, lastLng = 0.0;
static bool hasPrevPos = false;

static void gpsSetup() {
    if (!serialOk) {
        Serial1.begin(38400, SERIAL_8N1, BOARD_GPS_RX_PIN, BOARD_GPS_TX_PIN);
        serialOk = true;
    }
}

// Drain incoming NMEA bytes every call - called from tick() before any draw.
static void gpsPoll() {
    while (Serial1.available()) {
        gps.encode(Serial1.read());
    }
}

// Returns a string for date or "--" if not valid.
static String gpsDateStr() {
    if (!gps.date.isValid()) return "--/--/----";
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d/%02d/%04d",
             gps.date.day(), gps.date.month(), gps.date.year());
    return String(buf);
}

// Returns a string for UTC time or "--" if not valid.
static String gpsTimeStr() {
    if (!gps.time.isValid()) return "--:--:--";
    char buf[12];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
             gps.time.hour(), gps.time.minute(), gps.time.second());
    return String(buf);
}

static String gpsCourseStr() {
    if (!gps.course.isValid()) return "--";
    float c = gps.course.deg();
    // 8-direction label
    const char *dirs[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW", "N" };
    int idx = (int)((c + 22.5f) / 45.0f) % 8;
    char buf[24];
    snprintf(buf, sizeof(buf), "%.1f deg (%s)", c, dirs[idx]);
    return String(buf);
}

// ---- GPS Fix / Status screen ----
static void drawFix() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);

    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print("GPS Status  (MIA-M10Q)");

    int y = TOPBAR_HEIGHT + 22;

    // Satellites
    gfx->setTextColor(t.fg);
    gfx->setCursor(8, y);
    if (gps.satellites.isValid()) {
        gfx->print("Sats    : " + String(gps.satellites.value()));
    } else {
        gfx->print("Sats    : Searching...");
    }
    y += 18;

    // Location
    gfx->setCursor(8, y);
    if (gps.location.isValid()) {
        gfx->setTextColor(t.ok);
        gfx->print("Lat     : " + String(gps.location.lat(), 6));
    } else {
        gfx->setTextColor(t.warn);
        gfx->print("Lat     : Awaiting 3D fix...");
    }
    y += 16;

    gfx->setCursor(8, y);
    if (gps.location.isValid()) {
        gfx->setTextColor(t.ok);
        gfx->print("Lng     : " + String(gps.location.lng(), 6));
    } else {
        gfx->setTextColor(t.warn);
        gfx->print("Lng     : --");
    }
    y += 18;

    // Altitude
    gfx->setTextColor(t.fg);
    gfx->setCursor(8, y);
    gfx->print("Alt     : " + (gps.altitude.isValid() ? String(gps.altitude.meters(), 1) + " m" : "--"));
    y += 16;

    // Speed
    gfx->setCursor(8, y);
    gfx->print("Speed   : " + (gps.speed.isValid() ? String(gps.speed.kmph(), 1) + " km/h" : "--"));
    y += 16;

    // Course
    gfx->setCursor(8, y);
    gfx->print("Course  : " + gpsCourseStr());
    y += 16;

    // HDOP
    gfx->setCursor(8, y);
    gfx->print("HDOP    : " + (gps.hdop.isValid() ? String(gps.hdop.hdop(), 2) : "--"));
    y += 16;

    // Date / Time
    gfx->setTextColor(t.dim);
    gfx->setCursor(8, y);
    gfx->print("UTC     : " + gpsTimeStr() + "  " + gpsDateStr());
    y += 16;

    // Fix quality indicator bar at the bottom
    if (gps.satellites.isValid() && gps.satellites.value() > 0) {
        int bars = constrain((int)gps.satellites.value(), 0, 12);
        int barW = (SCREEN_W - 16) / 12;
        for (int i = 0; i < 12; i++) {
            uint16_t col = (i < bars) ? t.ok : t.dim;
            gfx->fillRect(8 + i * barW, SCREEN_H - 12, barW - 2, 8, col);
        }
        gfx->setTextColor(t.dim);
        gfx->setCursor(8, SCREEN_H - 24);
        gfx->print("Signal strength");
    }
}

// ---- Coordinate Tracker / Distance screen ----
static void drawTracker() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);

    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print("GPS Tracker");

    int y = TOPBAR_HEIGHT + 24;

    // Current position
    gfx->setTextColor(t.fg);
    gfx->setCursor(8, y);
    if (gps.location.isValid()) {
        gfx->setTextColor(t.ok);
        gfx->print("Pos : " + String(gps.location.lat(), 5) + ", " + String(gps.location.lng(), 5));
    } else {
        gfx->setTextColor(t.warn);
        gfx->print("Pos : Searching for fix...");
    }
    y += 18;

    // Speed
    gfx->setTextColor(t.fg);
    gfx->setCursor(8, y);
    float spd = gps.speed.isValid() ? gps.speed.kmph() : 0.0f;
    if (spd > maxSpeedKmh) maxSpeedKmh = spd;
    gfx->print("Speed   : " + (gps.speed.isValid() ? String(spd, 1) + " km/h" : "--"));
    y += 16;

    // Max speed
    gfx->setCursor(8, y);
    gfx->setTextColor(t.dim);
    gfx->print("Max spd : " + (maxSpeedKmh > 0 ? String(maxSpeedKmh, 1) + " km/h" : "--"));
    y += 16;

    // Total distance accumulated (accumulate when fix is valid)
    gfx->setTextColor(t.fg);
    gfx->setCursor(8, y);
    if (totalDistKm > 0) {
        gfx->print("Distance: " + String(totalDistKm, 3) + " km");
    } else {
        gfx->print("Distance: 0.000 km");
    }
    y += 16;

    // Altitude
    gfx->setCursor(8, y);
    gfx->print("Altitude: " + (gps.altitude.isValid() ? String(gps.altitude.meters(), 1) + " m" : "--"));
    y += 16;

    // Heading
    gfx->setCursor(8, y);
    gfx->print("Heading : " + gpsCourseStr());
    y += 16;

    // Satellites
    gfx->setTextColor(t.dim);
    gfx->setCursor(8, y);
    gfx->print("Sats    : " + (gps.satellites.isValid() ? String(gps.satellites.value()) : "--"));
    y += 14;

    gfx->setCursor(8, y);
    gfx->print("Time    : " + gpsTimeStr());
}

// ---- Compass / Heading screen ----
static void drawCompass() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);

    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print("GPS Heading / Compass");

    // Draw a simple compass rose in the centre
    int cx = SCREEN_W / 2;
    int cy = TOPBAR_HEIGHT + 100;
    int r  = 60;
    gfx->drawCircle(cx, cy, r, t.dim);
    gfx->drawCircle(cx, cy, r + 1, t.dim);

    // Cardinal labels
    gfx->setTextColor(t.fg);
    gfx->setCursor(cx - 3, cy - r - 12); gfx->print("N");
    gfx->setCursor(cx - 3, cy + r + 4);  gfx->print("S");
    gfx->setCursor(cx + r + 4, cy - 4);  gfx->print("E");
    gfx->setCursor(cx - r - 10, cy - 4); gfx->print("W");

    if (gps.course.isValid() && gps.speed.isValid() && gps.speed.kmph() > 0.5f) {
        float deg = gps.course.deg();
        float rad = (deg - 90.0f) * (3.14159f / 180.0f); // -90 because 0deg=north, screen 0=east
        int nx = (int)(cx + (r - 10) * cos(rad));
        int ny = (int)(cy + (r - 10) * sin(rad));
        // Draw heading needle
        gfx->drawLine(cx, cy, nx, ny, t.accent);
        gfx->fillCircle(nx, ny, 4, t.accent);
        gfx->fillCircle(cx, cy, 4, t.fg);

        gfx->setTextColor(t.ok);
        gfx->setCursor(8, TOPBAR_HEIGHT + 175);
        gfx->print("Heading : " + String(deg, 1) + " deg  " + gpsCourseStr());
    } else {
        gfx->fillCircle(cx, cy, 5, t.dim);
        gfx->setTextColor(t.warn);
        gfx->setCursor(8, TOPBAR_HEIGHT + 175);
        gfx->print("Need fix + motion for heading");
    }

    gfx->setTextColor(t.dim);
    gfx->setCursor(8, TOPBAR_HEIGHT + 190);
    gfx->print("Speed   : " + (gps.speed.isValid() ? String(gps.speed.kmph(), 1) + " km/h" : "--"));
}

// ---- GPS Logger screen ----
static void drawLogger() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);

    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print("GPS Track Logger");

    gfx->setTextColor(logActive ? t.ok : t.dim);
    gfx->setCursor(8, TOPBAR_HEIGHT + 24);
    gfx->print(logActive ? "Logging...  (OK=stop)" : "Press OK to start logging");

    gfx->setTextColor(t.fg);
    gfx->setCursor(8, TOPBAR_HEIGHT + 44);
    gfx->print("Points  : " + String(logCount));

    gfx->setCursor(8, TOPBAR_HEIGHT + 62);
    if (gps.location.isValid()) {
        gfx->setTextColor(t.ok);
        gfx->print("Pos     : " + String(gps.location.lat(), 5) + ", " + String(gps.location.lng(), 5));
    } else {
        gfx->setTextColor(t.warn);
        gfx->print("Pos     : Waiting for fix...");
    }

    gfx->setTextColor(t.dim);
    gfx->setCursor(8, TOPBAR_HEIGHT + 82);
    gfx->print("File    : " + (logPath.length() ? logPath : "(none)"));

    gfx->setCursor(8, TOPBAR_HEIGHT + 100);
    gfx->print("Format  : CSV (time,lat,lng,alt,spd)");

    gfx->setCursor(8, TOPBAR_HEIGHT + 118);
    gfx->print("Interval: every 5 seconds");
}

static void loggerStart() {
    if (logActive) return;
    char path[64];
    snprintf(path, sizeof(path), "/gps_logs/track_%lu.csv", (unsigned long)millis());
    SD.mkdir("/gps_logs");
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        logPath = "SD error";
        return;
    }
    f.println("time_utc,lat,lng,alt_m,speed_kmh,course_deg,sats");
    f.close();
    logPath = String(path);
    logCount = 0;
    logActive = true;
    lastLogMs = 0;
}

static void loggerStop() {
    logActive = false;
}

static void loggerTick() {
    if (!logActive) return;
    if (!gps.location.isValid()) return;
    if (millis() - lastLogMs < LOG_INTERVAL_MS) return;
    lastLogMs = millis();

    File f = SD.open(logPath.c_str(), FILE_APPEND);
    if (!f) return;

    char line[96];
    snprintf(line, sizeof(line), "%s,%.6f,%.6f,%.1f,%.1f,%.1f,%d",
        gpsTimeStr().c_str(),
        gps.location.lat(),
        gps.location.lng(),
        gps.altitude.isValid() ? gps.altitude.meters() : 0.0,
        gps.speed.isValid() ? gps.speed.kmph() : 0.0,
        gps.course.isValid() ? gps.course.deg() : 0.0,
        gps.satellites.isValid() ? (int)gps.satellites.value() : 0
    );
    f.println(line);
    f.close();
    logCount++;
}

// Accumulate distance while fix is valid and position has changed.
static void updateDistanceTracker() {
    if (!gps.location.isValid()) {
        hasPrevPos = false;
        return;
    }
    double lat = gps.location.lat();
    double lng = gps.location.lng();
    if (hasPrevPos && (lat != lastLat || lng != lastLng)) {
        double d = TinyGPSPlus::distanceBetween(lastLat, lastLng, lat, lng);
        totalDistKm += d / 1000.0;
    }
    lastLat = lat;
    lastLng = lng;
    hasPrevPos = true;
}

static void init() {
    exitApp = false;
    currentMode = MODE_MENU;
    gpsSetup();
    logActive = false;
    logCount = 0;
    logPath = "";
    maxSpeedKmh = 0.0f;
    totalDistKm = 0.0f;
    hasPrevPos = false;

    if (subSubMenu) delete subSubMenu;
    std::vector<MenuItem> items = {
        { "GPS Status / Fix",     ">", [](){ currentMode = MODE_FIX; drawFix(); } },
        { "Coordinate Tracker",   ">", [](){ currentMode = MODE_TRACKER; drawTracker(); } },
        { "Heading / Compass",    ">", [](){ currentMode = MODE_COMPASS; drawCompass(); } },
        { "Track Logger",         ">", [](){ currentMode = MODE_LOGGER; drawLogger(); } },
    };
    subSubMenu = new Menu("GPS Tools", items);
    subSubMenu->draw();
}

static void tick() {
    gpsPoll();
    updateDistanceTracker();
    loggerTick();

    if (currentMode == MODE_MENU && subSubMenu) {
        subSubMenu->tick();
        return;
    }

    if (millis() - lastDrawMs > 1000) {
        lastDrawMs = millis();
        switch (currentMode) {
            case MODE_FIX:     drawFix();     break;
            case MODE_TRACKER: drawTracker(); break;
            case MODE_COMPASS: drawCompass(); break;
            case MODE_LOGGER:  drawLogger();  break;
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
        if (currentMode == MODE_LOGGER && logActive) loggerStop();
        currentMode = MODE_MENU;
        audioClickBack();
        if (subSubMenu) subSubMenu->draw();
        return;
    }

    // Logger: OK toggles logging on/off
    if (currentMode == MODE_LOGGER && in.type == InputEvent::OK) {
        audioClickOk();
        if (logActive) loggerStop(); else loggerStart();
        drawLogger();
        return;
    }

    // Tracker: OK resets distance/max speed stats
    if (currentMode == MODE_TRACKER && in.type == InputEvent::OK) {
        audioClickOk();
        maxSpeedKmh = 0.0f;
        totalDistKm = 0.0f;
        hasPrevPos = false;
        drawTracker();
        return;
    }
}

static void onExit() {
    if (logActive) loggerStop();
    if (subSubMenu) {
        delete subSubMenu;
        subSubMenu = nullptr;
    }
}

static bool wantsExit() { return exitApp; }

}

AppModule gpsAppGet() {
    return { GpsApp::init, GpsApp::tick, GpsApp::handleInput, GpsApp::onExit, GpsApp::wantsExit };
}
