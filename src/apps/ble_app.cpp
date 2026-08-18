#include "app_api.h"
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include "display.h"
#include "settings.h"
#include "config.h"
#include "audio.h"
#include "menu.h"
#include "overlay.h"
#include "input.h"

namespace BleApp {

enum Mode {
    MODE_MENU = 0,
    MODE_SCANNER,
    MODE_BEACON,
    MODE_HID
};

// Standard composite keyboard+mouse HID report map (report ID 1 = keyboard,
// report ID 2 = mouse).
static const uint8_t hidReportDescriptor[] = {
    0x05, 0x01,       // USAGE_PAGE (Generic Desktop)
    0x09, 0x06,       // USAGE (Keyboard)
    0xa1, 0x01,       // COLLECTION (Application)
    0x85, 0x01,       //   REPORT_ID (1)
    0x05, 0x07,       //   USAGE_PAGE (Keyboard)
    0x19, 0xe0,       //   USAGE_MINIMUM (Left Control)
    0x29, 0xe7,       //   USAGE_MAXIMUM (Right GUI)
    0x15, 0x00,       //   LOGICAL_MINIMUM (0)
    0x25, 0x01,       //   LOGICAL_MAXIMUM (1)
    0x75, 0x01,       //   REPORT_SIZE (1)
    0x95, 0x08,       //   REPORT_COUNT (8)
    0x81, 0x02,       //   INPUT (modifier byte)
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x06,       //   REPORT_COUNT (6)
    0x75, 0x08,       //   REPORT_SIZE (8)
    0x15, 0x00, 0x25, 0x65,
    0x05, 0x07,
    0x19, 0x00, 0x29, 0x65,
    0x81, 0x00,       //   INPUT (key array, 6 bytes)
    0xc0,             // END_COLLECTION
    0x05, 0x01,       // USAGE_PAGE (Generic Desktop)
    0x09, 0x02,       // USAGE (Mouse)
    0xa1, 0x01,       // COLLECTION (Application)
    0x09, 0x01,       //   USAGE (Pointer)
    0xa1, 0x00,       //   COLLECTION (Physical)
    0x85, 0x02,       //     REPORT_ID (2)
    0x05, 0x09,       //     USAGE_PAGE (Button)
    0x19, 0x01, 0x29, 0x03,
    0x15, 0x00, 0x25, 0x01,
    0x95, 0x03, 0x75, 0x01,
    0x81, 0x02,       //     INPUT (3 buttons)
    0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
    0x05, 0x01,
    0x09, 0x30, 0x09, 0x31, 0x09, 0x38,
    0x15, 0x81, 0x25, 0x7f,
    0x75, 0x08, 0x95, 0x03,
    0x81, 0x06,       //     INPUT (X,Y,wheel - relative)
    0xc0,             //   END_COLLECTION
    0xc0              // END_COLLECTION
};

static Mode currentMode = MODE_MENU;
static Menu *subSubMenu = nullptr;
static bool exitApp = false;

struct BleDevInfo {
    String name;
    String addr;
    int rssi;
    String beaconInfo;
};

static std::vector<BleDevInfo> devList;
static int sel = 0;
static int scrollTop = 0;
static volatile bool scanning = false;
// scanJustFinished is only set from the BLE host task. It is cleared
// exclusively from the main task in tick(). This flag must only ever be
// acted on when we are still in a scanner/beacon mode. If the user
// navigated away before the scan completed, we discard the results.
static volatile bool scanJustFinished = false;

static bool bleReady = false;
static NimBLEScan *pScan = nullptr;

// --- BLE Remote Control (HID keyboard + mouse over GATT) ---
static bool hidReady = false;
static NimBLEHIDDevice *hid = nullptr;
static NimBLECharacteristic *inputKeyboard = nullptr;
static NimBLECharacteristic *inputMouse = nullptr;
static volatile bool hidConnected = false;
static bool hidLastConnected = false;
static bool savedTrackballCursor = false;

static void drawScanner();
static void drawBeacon();
static void drawHid();
static void ensureBleUp();

class HidServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *server, ble_gap_conn_desc *desc) override {
        hidConnected = true;
    }
    void onDisconnect(NimBLEServer *server, ble_gap_conn_desc *desc) override {
        hidConnected = false;
        // Restart advertising so the remote can reconnect without
        // re-entering this screen.
        NimBLEDevice::startAdvertising();
    }
};
static HidServerCallbacks hidServerCallbacks;

static void setupHid() {
    if (hidReady) return;
    ensureBleUp();

    NimBLEServer *server = NimBLEDevice::createServer();
    server->setCallbacks(&hidServerCallbacks);

    hid = new NimBLEHIDDevice(server);
    inputKeyboard = hid->inputReport(1);
    inputMouse    = hid->inputReport(2);

    hid->manufacturer(FW_NAME);
    hid->pnp(0x02, 0x05ac, 0x820a, 0x0210);
    hid->hidInfo(0x00, 0x01);
    hid->reportMap((uint8_t *)hidReportDescriptor, sizeof(hidReportDescriptor));
    hid->startServices();
    hid->setBatteryLevel(100);

    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->setAppearance(HID_KEYBOARD);
    adv->addServiceUUID(hid->hidService()->getUUID());
    adv->setScanResponse(true);

    hidReady = true;
}

static void enterHidMode() {
    setupHid();
    savedTrackballCursor = gSettings.trackballCursor;
    gSettings.trackballCursor = true;
    inputSetTextEntryMode(true);
    hidLastConnected = hidConnected;
    if (!hidConnected) NimBLEDevice::startAdvertising();
}

static void exitHidMode() {
    if (!hidReady) return;
    gSettings.trackballCursor = savedTrackballCursor;
    inputSetTextEntryMode(false);
    if (!hidConnected) NimBLEDevice::stopAdvertising();
}

static void sendKeyReport(uint8_t modifier, uint8_t keycode) {
    if (!hidConnected || !inputKeyboard) return;
    uint8_t report[8] = {0};
    report[0] = modifier;
    report[2] = keycode;
    inputKeyboard->setValue(report, sizeof(report));
    inputKeyboard->notify();
}

static void sendKeyRelease() {
    if (!hidConnected || !inputKeyboard) return;
    uint8_t report[8] = {0};
    inputKeyboard->setValue(report, sizeof(report));
    inputKeyboard->notify();
}

static void sendMouseReport(int8_t dx, int8_t dy, uint8_t buttons) {
    if (!hidConnected || !inputMouse) return;
    uint8_t report[4] = { buttons, (uint8_t)dx, (uint8_t)dy, 0 };
    inputMouse->setValue(report, sizeof(report));
    inputMouse->notify();
}

static uint8_t asciiToHidKeycode(char c, uint8_t &modifierOut) {
    modifierOut = 0;
    if (c >= 'a' && c <= 'z') return 0x04 + (c - 'a');
    if (c >= 'A' && c <= 'Z') { modifierOut = 0x02; return 0x04 + (c - 'A'); }
    if (c >= '1' && c <= '9') return 0x1e + (c - '1');
    if (c == '0')  return 0x27;
    if (c == ' ')  return 0x2c;
    if (c == '\n' || c == '\r') return 0x28;
    if (c == 0x08) return 0x2a;
    if (c == '.')  return 0x37;
    if (c == ',')  return 0x36;
    if (c == '-')  return 0x2d;
    return 0;
}

// The BLE stack comes up once per app session. Bringing it up and tearing it
// down on every scan was the original crash source.
static void ensureBleUp() {
    if (bleReady) return;
    // TEMP DIAGNOSTIC. Remove once the crash is confirmed/fixed.
    Serial.printf("[ble] free internal: %u  largest internal block: %u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    NimBLEDevice::init(FW_NAME);
    pScan = NimBLEDevice::getScan();
    pScan->setActiveScan(true);
    pScan->setInterval(45);
    pScan->setWindow(15);
    bleReady = true;
}

// Scan-complete callback runs on the NimBLE host task (small stack).
// Do nothing here except flip the flag. All heavy work in processScanResults().
static void onScanComplete(NimBLEScanResults /*results*/) {
    scanning = false;
    scanJustFinished = true;
}

// Called from tick() on the main task once scanJustFinished is set.
static void processScanResults() {
    devList.clear();
    if (!pScan) return;
    NimBLEScanResults results = pScan->getResults();
    int count = results.getCount();
    devList.reserve(count);
    for (int i = 0; i < count; i++) {
        NimBLEAdvertisedDevice dev = results.getDevice(i);
        BleDevInfo info;
        info.name = dev.getName().c_str();
        if (info.name.length() == 0) info.name = "(unknown)";
        info.addr = dev.getAddress().toString().c_str();
        info.rssi = dev.getRSSI();
        if (dev.haveManufacturerData()) {
            std::string mData = dev.getManufacturerData();
            info.beaconInfo = "Mfg Data (" + String((int)mData.length()) + "B)";
        } else {
            info.beaconInfo = "Standard Advert";
        }
        devList.push_back(info);
    }
    sel = 0;
    scrollTop = 0;
}

static void doScan() {
    if (scanning) return;
    ensureBleUp();
    scanning = true;
    scanJustFinished = false;
    devList.clear();
    showLoadingOverlay("Scanning BLE...");
    // 3-second async scan; onScanComplete fires on the BLE host task.
    pScan->start(3, onScanComplete, false);
}

static void drawScanner() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);
    gfx->setTextSize(1);
    gfx->setTextColor(t.dim);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print(scanning ? "Scanning BLE..."
                         : (String(devList.size()) + " devices  (OK=rescan  BACK=menu)"));

    int visibleRows = (SCREEN_H - TOPBAR_HEIGHT - 20) / 18;
    for (int row = 0; row < visibleRows; row++) {
        int i = scrollTop + row;
        if (i >= (int)devList.size()) break;
        int y = TOPBAR_HEIGHT + 22 + row * 18;
        bool hi = (i == sel);
        if (hi) gfx->fillRect(4, y - 2, SCREEN_W - 8, 16, t.accent);
        gfx->setTextColor(hi ? t.accentFg : t.fg);
        gfx->setCursor(8, y);
        gfx->print(devList[i].name.substring(0, 14) + " " +
                   String(devList[i].rssi) + "dBm " +
                   devList[i].addr.substring(0, 11));
    }
}

static void drawBeacon() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);
    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print(scanning ? "Scanning BLE..." : "BLE Beacon & Mfg Monitor");

    int visibleRows = (SCREEN_H - TOPBAR_HEIGHT - 20) / 18;
    for (int row = 0; row < visibleRows; row++) {
        int i = scrollTop + row;
        if (i >= (int)devList.size()) break;
        int y = TOPBAR_HEIGHT + 22 + row * 18;
        bool hi = (i == sel);
        if (hi) gfx->fillRect(4, y - 2, SCREEN_W - 8, 16, t.accent);
        gfx->setTextColor(hi ? t.accentFg : t.fg);
        gfx->setCursor(8, y);
        gfx->print(devList[i].addr.substring(0, 12) + " | " + devList[i].beaconInfo);
    }
}

static void drawHid() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);
    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print("BLE Remote Control");

    gfx->setTextColor(hidConnected ? t.ok : t.dim);
    gfx->setCursor(8, TOPBAR_HEIGHT + 24);
    gfx->print(hidConnected ? "Connected" : "Advertising — pair from your device");

    gfx->setTextColor(t.fg);
    gfx->setCursor(8, TOPBAR_HEIGHT + 50);
    gfx->print("Keyboard  -> sends keystrokes");
    gfx->setCursor(8, TOPBAR_HEIGHT + 68);
    gfx->print("Trackball -> moves mouse");
    gfx->setCursor(8, TOPBAR_HEIGHT + 86);
    gfx->print("TB click  -> left mouse click");
    gfx->setCursor(8, TOPBAR_HEIGHT + 104);
    gfx->print("BACK      -> exit");
}

static void init() {
    exitApp = false;
    currentMode = MODE_MENU;

    if (subSubMenu) delete subSubMenu;
    std::vector<MenuItem> items = {
        { "BLE Device Scanner", ">", [](){ currentMode = MODE_SCANNER; doScan(); } },
        { "Beacon Monitor",     ">", [](){ currentMode = MODE_BEACON;  doScan(); } },
        { "Remote Control",     ">", [](){ currentMode = MODE_HID; enterHidMode(); drawHid(); } }
    };
    subSubMenu = new Menu("BLE Tools", items);
    subSubMenu->draw();
}

static void tick() {
    if (currentMode == MODE_MENU && subSubMenu) {
        subSubMenu->tick();
        return;
    }

    // Pick up scan results on the main task. Safe for heavy String/vector work.
    // Only process them if we're still on a scanner/beacon screen; discard if
    // the user navigated elsewhere while the scan was in flight.
    if (scanJustFinished) {
        scanJustFinished = false;
        if (currentMode == MODE_SCANNER || currentMode == MODE_BEACON) {
            processScanResults();
            hideLoadingOverlay();
            if (currentMode == MODE_SCANNER) drawScanner();
            else drawBeacon();
        } else {
            // Navigated away before results arrived. Just clean up the overlay
            // if it somehow got left showing (normally hideLoadingOverlay is
            // idempotent when not active, so this is safe).
            hideLoadingOverlay();
        }
    }

    if (currentMode == MODE_HID && hidConnected != hidLastConnected) {
        hidLastConnected = hidConnected;
        drawHid();
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
        if (currentMode == MODE_HID) exitHidMode();
        // If a scan is still running and the user backs out, don't crash.
        // The scan result will be discarded in tick() once it completes.
        currentMode = MODE_MENU;
        audioClickBack();
        if (subSubMenu) subSubMenu->draw();
        return;
    }

    if (currentMode == MODE_HID) {
        if (in.type == InputEvent::CHAR) {
            uint8_t modifier = 0;
            uint8_t keycode = asciiToHidKeycode(in.ch, modifier);
            if (keycode != 0) {
                sendKeyReport(modifier, keycode);
                delay(8);
                sendKeyRelease();
            }
        } else if (in.type == InputEvent::CURSOR_MOVE) {
            int8_t dx = (int8_t)constrain(in.dx, -127, 127);
            int8_t dy = (int8_t)constrain(in.dy, -127, 127);
            sendMouseReport(dx, dy, 0);
        } else if (in.type == InputEvent::OK) {
            sendMouseReport(0, 0, 0x01);
            delay(8);
            sendMouseReport(0, 0, 0x00);
        }
        return;
    }

    // Block navigation input while a scan is in progress.
    if (scanning) return;

    if (currentMode == MODE_SCANNER || currentMode == MODE_BEACON) {
        int visibleRows = (SCREEN_H - TOPBAR_HEIGHT - 20) / 18;
        if (in.type == InputEvent::NAV_UP) {
            if (sel > 0) {
                sel--;
                if (sel < scrollTop) scrollTop = sel;
                audioClickNav();
                if (currentMode == MODE_SCANNER) drawScanner(); else drawBeacon();
            }
        } else if (in.type == InputEvent::NAV_DOWN) {
            if (sel < (int)devList.size() - 1) {
                sel++;
                if (sel >= scrollTop + visibleRows) scrollTop = sel - visibleRows + 1;
                audioClickNav();
                if (currentMode == MODE_SCANNER) drawScanner(); else drawBeacon();
            }
        } else if (in.type == InputEvent::OK) {
            audioClickOk();
            doScan();
        }
    }
}

static void onExit() {
    if (subSubMenu) {
        delete subSubMenu;
        subSubMenu = nullptr;
    }

    exitHidMode();

    if (scanning) {
        if (pScan) pScan->stop();
        // pScan->stop() is async. Give the host task a bounded window to
        // deliver the completion event and flip scanning=false before we
        // tear the stack down underneath it. This prevents the
        // use-after-free that was the remaining crash source.
        uint32_t waitStart = millis();
        while (scanning && (millis() - waitStart) < 300) {
            delay(5);
        }
        scanning = false;
        scanJustFinished = false;
        hideLoadingOverlay();
    }

    if (bleReady) {
        NimBLEDevice::deinit(true);
        bleReady = false;
        pScan = nullptr;
    }

    if (hidReady) {
        delete hid;
        hid = nullptr;
        inputKeyboard = nullptr;
        inputMouse = nullptr;
        hidConnected = false;
        hidReady = false;
    }
}

static bool wantsExit() { return exitApp; }

}

AppModule bleAppGet() {
    return { BleApp::init, BleApp::tick, BleApp::handleInput, BleApp::onExit, BleApp::wantsExit };
}
