#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <nvs_flash.h>
#include "pins.h"
#include "config.h"
#include "settings.h"
#include "display.h"
#include "input.h"
#include "cursor.h"
#include "audio.h"
#include "battery.h"
#include "splash.h"
#include "menu.h"
#include "app_api.h"
#include "overlay.h"
#include "zapp_loader.h"

// Shared SPI bus used by display, SD, and LoRa (only one CS active at a time).
SPIClass *gSharedSPI = &SPI;

AppModule *gActiveApp = nullptr;
AppModule gAppTable[11]; // index 10 = zappRunnerAppGet(), launched by files_app.cpp's My Apps screen, not root_menu.cpp

static bool sdOk = false;
static uint32_t lastSdCheckMs = 0;
static uint32_t lastFlushMs = 0;
static bool screenBacklightOn = true;
static uint32_t lastActivityMs = 0;

// The canvas (see display.cpp) doesn't show anything on the physical panel
// until flushed - flush() is a full 320x240x16bpp SPI burst (~150KB), so
// this is throttled to a sane cap (~60fps) rather than firing on every
// loop() iteration (which runs essentially unthrottled, every ~2ms). Any
// draws that happened since the last flush are still shown, just batched
// together into the next flush instead of each getting its own SPI burst -
// which is the whole point: only ever-complete frames reach the screen.
static void flushDisplay() {
    if (millis() - lastFlushMs < 16) return;
    lastFlushMs = millis();
    gfx->flush();
}

static void handleScreenTimeout() {
    if (gSettings.screenTimeoutSec == 0) {
        if (!screenBacklightOn) {
            displaySetBacklight(gSettings.brightness);
            screenBacklightOn = true;
        }
        return;
    }

    if (screenBacklightOn && (millis() - lastActivityMs >= (uint32_t)gSettings.screenTimeoutSec * 1000U)) {
        displaySetBacklight(0);
        screenBacklightOn = false;
    }
}

static void touchScreenActivity() {
    lastActivityMs = millis();
    if (!screenBacklightOn) {
        displaySetBacklight(gSettings.brightness);
        screenBacklightOn = true;
    }
}

// Re-checks the card is still readable (SD.begin() only ever runs once, at
// boot, so without this we'd never notice the card being pulled). Also
// doubles as the alert overlay's resolvedCheck. it's what lets the "SD Card
// Removed" alert clear itself early the instant the card is reinserted,
// instead of always sitting there for the full 5 seconds.
static bool sdCardPresent() {
    File root = SD.open("/");
    bool ok = (bool)root;
    if (root) root.close();
    return ok;
}

// Called once the "SD Card Removed" alert clears (either because the card
// came back, or the 5s timeout expired without it). Syncs sdOk either way and
// forces a redraw of whatever the alert had blanked over.
static void onSdAlertDismissed() {
    sdOk = sdCardPresent();
    // The alert overlay just blanked/repainted the whole screen out from
    // under the cursor sprite without cursor.cpp knowing - forget its saved
    // backing rather than restoring stale pre-alert pixels over the fresh
    // redraw below on the next cursorTick(). See cursor.h.
    cursorInvalidate();
    if (!gActiveApp) rootMenuForceRedraw();
    // If an app was active, its own next tick()/input-driven redraw will
    // repaint it. most app screens redraw on their own tick() cadence
    // already (scanners, monitors, etc.), same as they would after any other
    // momentary interruption.
}

static void deselectAllSpiDevices() {
    pinMode(BOARD_SDCARD_CS, OUTPUT); digitalWrite(BOARD_SDCARD_CS, HIGH);
    pinMode(BOARD_TFT_CS, OUTPUT);    digitalWrite(BOARD_TFT_CS, HIGH);
    pinMode(RADIO_CS_PIN, OUTPUT);    digitalWrite(RADIO_CS_PIN, HIGH);
}

void setup() {
    Serial.begin(115200);
    delay(300); // settle for the native-USB CDC link to enumerate, or so i've been told
    Serial.println("\n=== Zyro-Lite BOOT ===");

    // NVS has to be valid before anything touches BLE: NimBLE's controller
    // reads/writes Bluetooth calibration data from the NVS partition on
    // init (esp_bt_controller_init), and a blank or out-of-date partition
    // makes that call hard-fault the whole chip rather than fail cleanly -
    // this is what was actually behind "every BLE screen reboots the
    // device", not anything in ble_app.cpp itself. Settings moved off NVS
    // onto /zyro.conf a while back (see settings.cpp), so nothing else in
    // this firmware ever initializes NVS anymore - the BLE app was the
    // only thing left that ever touched the partition, and it was never
    // being formatted/mounted first.
    esp_err_t nvsErr = nvs_flash_init();
    if (nvsErr == ESP_ERR_NVS_NO_FREE_PAGES || nvsErr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvsErr = nvs_flash_init();
    }
    Serial.printf("[boot] nvs_flash_init: %s\n", nvsErr == ESP_OK ? "OK" : "FAIL");

    pinMode(BOARD_POWERON, OUTPUT);
    digitalWrite(BOARD_POWERON, HIGH);

    SPI.begin(BOARD_SPI_SCK, BOARD_SPI_MISO, BOARD_SPI_MOSI);
    deselectAllSpiDevices();

    // SD has to come up BEFORE settingsLoad() now: settings live in
    // /zyro.conf on the card instead of NVS. If there's no card, or no
    // config file yet, settingsLoad() fails soft and just keeps the in-RAM
    // defaults (see settings.cpp).
    sdOk = SD.begin(BOARD_SDCARD_CS, SPI);
    Serial.printf("[boot] SD.begin: %s\n", sdOk ? "OK" : "not present");

    settingsLoad();

    displayInit(); // uses gSettings.brightness - must come after settingsLoad()
    screenBacklightOn = true;
    lastActivityMs = millis();
    Serial.println("[boot] displayInit done");

    inputInit(); // uses gSettings.keyboardBacklight - must come after settingsLoad()
    cursorInit(); // must come after displayInit() (see cursor.h)
    audioInit();
    batteryInit();

    showSplash(sdOk);

    // App table
    gAppTable[0] = settingsAppGet();
    gAppTable[1] = wifiAppGet();
    gAppTable[2] = bleAppGet();
    gAppTable[3] = loraAppGet();
    gAppTable[4] = rfAppGet();
    gAppTable[5] = gpsAppGet();
    gAppTable[6] = ethernetAppGet();
    gAppTable[7] = gamesAppGet();
    gAppTable[8] = filesAppGet();
    gAppTable[9] = gpioAppGet();
    gAppTable[10] = zappRunnerAppGet();

    // If the device rebooted mid-.zApp last session (crash/hang forced a
    // reset), flag it here before anything else touches the menu - see
    // zapp_loader.cpp for why this can only be detected on the NEXT boot.
    zappCheckCrashGuard();

    rootMenuInit();
    
    // Clear any input events that happened during boot/splash screen
    // (e.g. from holding the boot button to flash the firmware).
    while (inputPoll().type != InputEvent::NONE) { delay(1); }
    
    Serial.println("[boot] rootMenuInit done - reached loop()");
}

void loop() {
    static uint32_t lastTopbarMs = 0;

    // Both overlays are no-ops when not active, so these are cheap to call unconditionally every iteration.
    loadingOverlayTick();
    alertTick();

    handleScreenTimeout();

    // Re-check the SD card periodically (SD.begin() in setup() only runs
    // once, so this is the only thing that notices the card being pulled).
    if (millis() - lastSdCheckMs > 1000) {
        lastSdCheckMs = millis();
        bool nowPresent = sdCardPresent();
        if (sdOk && !nowPresent) {
            sdOk = false;
            showAlert("SD Card Removed", sdCardPresent, onSdAlertDismissed);
        }
    }

    // While the alert overlay is up, it owns the whole screen - skip normal
    // input/tick handling underneath it entirely so nothing changes state or
    // draws over top of it. Once it clears (see onSdAlertDismissed), normal
    // handling resumes and repaints whatever's actually current.
    if (alertActive()) {
        flushDisplay(); // alertTick() above may have just drawn a new frame
        delay(gSettings.batterySaver ? 15 : 2);
        return;
    }

    InputResult in = inputPoll();
    if (in.type != InputEvent::NONE) {
        touchScreenActivity();
    }

    // Debug: log any input event to serial
    if (in.type != InputEvent::NONE) {
        Serial.printf("[input] event=%d ch=%c\n", (int)in.type, in.ch ? in.ch : '?');
    }

    // Route the input to whichever side owns it right now, but don't decide
    // who ticks next until after that - handleInput() can itself launch an
    // app (root menu selecting something) or close one (app requesting
    // exit), and gActiveApp reflects that change immediately. Ticking based
    // on the value from before handleInput() ran meant a freshly launched
    // app's first frame got immediately painted over by one extra
    // rootMenuTick() call still targeting the old (root) side, since that
    // branch had already been chosen before the launch happened. That's the
    // "app icon/name still shows until you press a key" glitch.
    if (in.type == InputEvent::CURSOR_MOVE) {
        // Trackball: Cursor mode - move the on-screen pointer instead of
        // routing this as menu/app navigation.
        cursorMove(in.dx, in.dy);
    } else if (gActiveApp) {
        if (in.type != InputEvent::NONE) gActiveApp->handleInput(in);
    } else {
        if (in.type != InputEvent::NONE) rootMenuHandleInput(in);
    }

    if (gActiveApp) {
        gActiveApp->tick();
        if (gActiveApp->wantsExit()) {
            gActiveApp->onExit();
            gActiveApp = nullptr;
            rootMenuInit(); // redraw the launcher fresh
        }
    } else {
        rootMenuTick();
    }

    // Topbar refresh (battery / SD / Wi-Fi icon)
    if (millis() - lastTopbarMs > 1000) {
        lastTopbarMs = millis();
        drawTopbar(batteryPercent(), batteryCharging(), sdOk);
    }

    // Composite/move the on-screen cursor sprite, after all other drawing
    // for this frame, right before it's pushed to the panel. No-ops when
    // Trackball: Cursor mode is off. See cursor.h.
    cursorTick();

    flushDisplay();

    // Always yield to the RTOS idle task / watchdog timer.
    // Without this, the tight loop starves the WiFi/BT background tasks
    // and eventually triggers a task watchdog reset (~6 seconds).
    delay(gSettings.batterySaver ? 15 : 2);
}
