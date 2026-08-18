#include "zapp_loader.h"
#include "zyro_sdk_api.h"
#include "zapp_registry.h"
#include <SD.h>
#include <WiFi.h>
#include <NimBLEDevice.h>
#include <RadioLib.h>
#include <esp_heap_caps.h>
#include <setjmp.h>
#include "display.h"
#include "config.h"
#include "settings.h"
#include "battery.h"
#include "pins.h"
#include "overlay.h"
#include "menu.h" // rootMenuForceRedraw()

extern SPIClass *gSharedSPI;

// ============================================================================
// Everything in this file is the ONLY code that ever touches a .zApp's
// bytes. See zapp_loader.h for the overall design rationale.
// ============================================================================

// ---- reboot-survives crash guard -----------------------------------------
// RTC slow memory keeps its contents across a software reset/panic (though
// not a full power-cycle) - exactly what's needed to notice "the last thing
// that happened before this boot was a zApp running" after the reset has
// already happened and every other RAM variable is gone.
#define ZAPP_GUARD_MAGIC 0x5A41504Cu // "ZAPL"
RTC_NOINIT_ATTR static uint32_t gZappGuardMagic;
RTC_NOINIT_ATTR static char gZappGuardName[ZAPP_NAME_MAX + 1];

static void armCrashGuard(const String &name) {
    gZappGuardMagic = ZAPP_GUARD_MAGIC;
    strncpy(gZappGuardName, name.c_str(), ZAPP_NAME_MAX);
    gZappGuardName[ZAPP_NAME_MAX] = 0;
}
static void disarmCrashGuard() {
    gZappGuardMagic = 0;
}

void zappCheckCrashGuard() {
    if (gZappGuardMagic != ZAPP_GUARD_MAGIC) { gZappGuardMagic = 0; return; }
    String name(gZappGuardName);
    gZappGuardMagic = 0;
    if (name.length() > 0) zappRegistrySetBlocked(name, true);
    showAlert("Error, Review your app on another device", nullptr, rootMenuForceRedraw);
}

// ---- soft (in-app) fault containment -------------------------------------
// longjmp, not a C++ exception: the loaded object is a bare relocated blob
// with no linked unwind tables of its own, so throwing across that boundary
// is not something to rely on. setjmp/longjmp only touches the stack
// pointer/registers, so it works regardless of what compiled the code on
// either side. Every call INTO app code goes through callGuarded() below;
// nothing but a plain function-pointer invocation ever sits between the
// setjmp and the longjmp target.
static jmp_buf gZappJumpBuf;
static bool gZappFaulted = false;
static char gZappFaultMsg[64];

static void reportErrorImpl(const char *msg) {
    if (msg) { strncpy(gZappFaultMsg, msg, sizeof(gZappFaultMsg) - 1); gZappFaultMsg[sizeof(gZappFaultMsg) - 1] = 0; }
    longjmp(gZappJumpBuf, 1);
}

// ---- loaded-app state ------------------------------------------------------
static ZyroAppModule gLoaded = {};
static bool gAppLoaded = false;
static bool gExitRequested = false;
static String gPendingAppName;
static String gRunningAppName;

static uint8_t *gTextBuf = nullptr;
static uint8_t *gDataBuf = nullptr; // holds rodata+data+bss back to back

// runs fn() under the fault guard; returns false if it faulted (reportError
// or a caught C++ exception) - caller should treat that as "stop now".
static bool callGuarded(void (*fn)()) {
    if (!fn) return true;
    if (setjmp(gZappJumpBuf) != 0) { gZappFaulted = true; return false; }
    try {
        fn();
    } catch (...) {
        strncpy(gZappFaultMsg, "uncaught exception", sizeof(gZappFaultMsg) - 1);
        gZappFaulted = true;
        return false;
    }
    return true;
}

// =============================== ZyroApi ===================================
// Every function below is what a .zApp can actually reach. Keep this list
// the sandbox boundary - don't add anything here that reaches outside the
// app's own canvas rect / sandbox folder / the existing pin allowlist.

// --- canvas: always offset so (0,0) == first pixel below the topbar, and
// clamped so nothing can be drawn back up into the topbar strip itself. ---
static inline int cvY(int y) { return y + TOPBAR_HEIGHT; }

static void api_fillRect(int x, int y, int w, int h, uint16_t c) {
    if (y < 0) { h += y; y = 0; }
    if (h <= 0 || w <= 0) return;
    gfx->fillRect(x, cvY(y), w, h, c);
}
static void api_drawRect(int x, int y, int w, int h, uint16_t c) { if (y < 0) return; gfx->drawRect(x, cvY(y), w, h, c); }
static void api_drawPixel(int x, int y, uint16_t c) { if (y < 0) return; gfx->drawPixel(x, cvY(y), c); }
static void api_drawLine(int x0, int y0, int x1, int y1, uint16_t c) { if (y0 < 0 || y1 < 0) return; gfx->drawLine(x0, cvY(y0), x1, cvY(y1), c); }
static void api_setCursor(int x, int y) { if (y < 0) y = 0; gfx->setCursor(x, cvY(y)); }
static void api_setTextColor(uint16_t c) { gfx->setTextColor(c); }
static void api_print(const char *s) { if (s) gfx->print(s); }

// --- file: sandboxed to /apps/<running app>/data/ --------------------------
static int api_readAll(const char *relPath, uint8_t *outBuf, size_t maxLen) {
    String full = zappSandboxResolve(gRunningAppName, relPath);
    if (full.length() == 0) return -1;
    File f = SD.open(full, FILE_READ);
    if (!f) return -1;
    int n = f.read(outBuf, maxLen);
    f.close();
    return n;
}
static bool api_writeAll(const char *relPath, const uint8_t *data, size_t len) {
    String full = zappSandboxResolve(gRunningAppName, relPath);
    if (full.length() == 0) return false;
    SD.remove(full);
    File f = SD.open(full, FILE_WRITE);
    if (!f) return false;
    size_t n = f.write(data, len);
    f.close();
    return n == len;
}
static bool api_exists(const char *relPath) {
    String full = zappSandboxResolve(gRunningAppName, relPath);
    return full.length() > 0 && SD.exists(full);
}
static bool api_remove(const char *relPath) {
    String full = zappSandboxResolve(gRunningAppName, relPath);
    return full.length() > 0 && SD.remove(full);
}

// --- wifi --------------------------------------------------------------
static bool api_wifiScan(char names[][33], int maxNetworks, int *outCount) {
    WiFi.mode(WIFI_STA);
    int n = WiFi.scanNetworks();
    int count = 0;
    for (int i = 0; i < n && count < maxNetworks; i++) {
        String ssid = WiFi.SSID(i);
        strncpy(names[count], ssid.c_str(), 32);
        names[count][32] = 0;
        count++;
    }
    WiFi.scanDelete();
    if (outCount) *outCount = count;
    return true;
}
static bool api_wifiConnect(const char *ssid, const char *pass, uint32_t timeoutMs) {
    if (!ssid) return false;
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) delay(50);
    return WiFi.status() == WL_CONNECTED;
}
static bool api_wifiIsConnected() { return WiFi.status() == WL_CONNECTED; }
static void api_wifiDisconnect() { WiFi.disconnect(true); }

// --- ble -----------------------------------------------------------------
static NimBLEScan *gBleScan = nullptr;
static bool api_bleScan(char names[][33], int maxDevices, int *outCount, uint32_t scanMs) {
    if (!NimBLEDevice::getInitialized()) NimBLEDevice::init("");
    gBleScan = NimBLEDevice::getScan();
    // NimBLE-Arduino's blocking start() takes whole seconds, not ms - this
    // library version has no getResults(ms, bool) overload (older API only).
    uint32_t scanSec = scanMs / 1000;
    if (scanSec == 0) scanSec = 1;
    NimBLEScanResults results = gBleScan->start(scanSec, false);
    int count = 0;
    for (int i = 0; i < results.getCount() && count < maxDevices; i++) {
        std::string nm = results.getDevice(i).getName();
        if (nm.empty()) nm = results.getDevice(i).getAddress().toString();
        strncpy(names[count], nm.c_str(), 32);
        names[count][32] = 0;
        count++;
    }
    if (outCount) *outCount = count;
    return true;
}
static bool api_bleAdvertiseStart(const char *name) {
    if (!NimBLEDevice::getInitialized()) NimBLEDevice::init(name ? name : "Zyro-App");
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->start();
    return true;
}
static void api_bleAdvertiseStop() {
    if (NimBLEDevice::getInitialized()) NimBLEDevice::getAdvertising()->stop();
}

// --- lora ------------------------------------------------------------------
// Owns its own SX1262 instance, same "each app initializes its own radio
// fresh" pattern as lora_app.cpp / rf_app.cpp - never shares state with them.
static Module *gLoraModule = nullptr;
static SX1262 *gLoraRadio = nullptr;
static bool gLoraOk = false;
static bool loraEnsure() {
    if (gLoraOk) return true;
    gLoraModule = new Module(RADIO_CS_PIN, RADIO_DIO1_PIN, RADIO_RST_PIN, RADIO_BUSY_PIN, *gSharedSPI);
    gLoraRadio = new SX1262(gLoraModule);
    gLoraOk = (gLoraRadio->begin(868.0, 125.0, 7, 5, 0x12, 10, 8, 1.6, false) == RADIOLIB_ERR_NONE);
    return gLoraOk;
}
static bool api_loraAvailable() { return loraEnsure(); }
static bool api_loraSend(const uint8_t *data, size_t len) {
    if (!loraEnsure()) return false;
    return gLoraRadio->transmit((uint8_t *)data, len) == RADIOLIB_ERR_NONE;
}
static int api_loraReceive(uint8_t *outBuf, size_t maxLen, uint32_t timeoutMs) {
    if (!loraEnsure()) return -1;
    gLoraRadio->startReceive();
    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        String s;
        if (gLoraRadio->available() && gLoraRadio->readData(s) == RADIOLIB_ERR_NONE) {
            size_t n = s.length() < maxLen ? s.length() : maxLen;
            memcpy(outBuf, s.c_str(), n);
            return (int)n;
        }
        delay(5);
    }
    return 0;
}

// --- gpio: index-based into the existing GPIO_CUSTOM_PINS allowlist -------
static int  api_gpioPinCount() { return (int)GPIO_CUSTOM_PIN_COUNT; }
static bool api_gpioRead(int idx) {
    if (idx < 0 || idx >= (int)GPIO_CUSTOM_PIN_COUNT) return false;
    pinMode(GPIO_CUSTOM_PINS[idx], INPUT);
    return digitalRead(GPIO_CUSTOM_PINS[idx]);
}
static void api_gpioWrite(int idx, bool high) {
    if (idx < 0 || idx >= (int)GPIO_CUSTOM_PIN_COUNT) return;
    pinMode(GPIO_CUSTOM_PINS[idx], OUTPUT);
    digitalWrite(GPIO_CUSTOM_PINS[idx], high ? HIGH : LOW);
}

// --- system ----------------------------------------------------------------
static uint32_t api_millis() { return millis(); }
static int  api_battPct() { return batteryPercent(); }
static bool api_battChg() { return batteryCharging(); }
static void api_log(const char *msg) { if (msg) Serial.printf("[zApp:%s] %s\n", gRunningAppName.c_str(), msg); }
static void api_reportError(const char *msg) { reportErrorImpl(msg); }

static ZyroApi gApi = {
    ZYRO_SDK_API_VERSION,
    { SCREEN_W, SCREEN_H - TOPBAR_HEIGHT,
      api_fillRect, api_drawRect, api_drawPixel, api_drawLine,
      api_setCursor, api_setTextColor, api_print },
    { api_writeAll, api_readAll, api_exists, api_remove },
    { api_wifiScan, api_wifiConnect, api_wifiIsConnected, api_wifiDisconnect },
    { api_bleScan, api_bleAdvertiseStart, api_bleAdvertiseStop },
    { api_loraAvailable, api_loraSend, api_loraReceive },
    { api_gpioPinCount, api_gpioRead, api_gpioWrite },
    { api_millis, api_battPct, api_battChg, api_log, api_reportError }
};

// ============================ .zApp load/unload =============================

static void freeAppMemory() {
    if (gTextBuf) { heap_caps_free(gTextBuf); gTextBuf = nullptr; }
    if (gDataBuf) { heap_caps_free(gDataBuf); gDataBuf = nullptr; }
    gLoraOk = false; // release the lora radio object handles too
    if (gLoraRadio) { delete gLoraRadio; gLoraRadio = nullptr; }
    if (gLoraModule) { delete gLoraModule; gLoraModule = nullptr; }
}

static void failLoad(const char *why) {
    Serial.printf("[zapp] load failed: %s\n", why);
    freeAppMemory();
    gAppLoaded = false;
    gExitRequested = true;
    showAlert("Error, Review your app on another device", nullptr, rootMenuForceRedraw);
}

static bool loadZapp(const String &appName) {
    String path = String(ZAPP_DIR) + "/" + appName + "/" + appName + ".zApp";
    File f = SD.open(path, FILE_READ);
    if (!f) { failLoad("file not found"); return false; }

    ZappHeader hdr;
    if (f.read((uint8_t *)&hdr, sizeof(hdr)) != sizeof(hdr)) { f.close(); failLoad("truncated header"); return false; }
    if (hdr.magic != ZAPP_MAGIC) { f.close(); failLoad("bad magic"); return false; }
    if (hdr.apiVersion != ZYRO_SDK_API_VERSION) { f.close(); failLoad("SDK version mismatch"); return false; }
    if (hdr.textSize == 0 || hdr.textSize > ZAPP_TEXT_MAX) { f.close(); failLoad("text size out of range"); return false; }
    uint32_t rdSize = hdr.rodataSize + hdr.dataSize + hdr.bssSize;
    if (rdSize > ZAPP_RODATA_DATA_MAX) { f.close(); failLoad("data size out of range"); return false; }
    if (hdr.entryOffset >= hdr.textSize) { f.close(); failLoad("bad entry offset"); return false; }

    std::vector<ZappReloc> relocs(hdr.relocCount);
    if (hdr.relocCount > 0) {
        size_t need = hdr.relocCount * sizeof(ZappReloc);
        if (f.read((uint8_t *)relocs.data(), need) != need) { f.close(); failLoad("truncated relocations"); return false; }
    }

    uint32_t textAllocSize = (hdr.textSize + 3u) & ~3u; // round up: the trailing word-store below must never overrun the buffer
    gTextBuf = (uint8_t *)heap_caps_malloc(textAllocSize, MALLOC_CAP_EXEC | MALLOC_CAP_32BIT);
    gDataBuf = (uint8_t *)heap_caps_malloc(rdSize > 0 ? rdSize : 4, MALLOC_CAP_SPIRAM);
    if (!gTextBuf || !gDataBuf) { f.close(); failLoad("out of memory"); return false; }

    // gTextBuf is IRAM (MALLOC_CAP_EXEC): the hardware only permits 32-bit-
    // aligned loads/stores there. Handing it straight to File::read() (as
    // this used to) lets the SD library's own memcpy/byte-copy do a stray
    // byte access into IRAM, which raises a LoadStoreError exception and
    // reboots the whole device - not something setjmp/longjmp or any C-level
    // guard ever gets a chance to see, since it's a hardware trap, not a
    // function call. So text bytes are staged through ordinary DRAM first,
    // then copied into gTextBuf a 32-bit word at a time below.
    bool ok = true;
    {
        static uint8_t stage[512];
        uint32_t remaining = hdr.textSize;
        uint32_t written = 0;
        while (remaining > 0 && ok) {
            uint32_t chunk = remaining < sizeof(stage) ? remaining : sizeof(stage);
            if (f.read(stage, chunk) != chunk) { ok = false; break; }
            uint32_t off = 0;
            // sizeof(stage) is a multiple of 4, so 'written' stays word-
            // aligned across chunks; only the final (possibly short) chunk
            // can leave a <4-byte remainder, handled below.
            while (off + 4 <= chunk) {
                uint32_t v; memcpy(&v, stage + off, 4);
                *(uint32_t *)(gTextBuf + written + off) = v;
                off += 4;
            }
            if (off < chunk) {
                uint32_t v = 0;
                memcpy(&v, stage + off, chunk - off);
                *(uint32_t *)(gTextBuf + written + off) = v;
            }
            written += chunk;
            remaining -= chunk;
        }
    }
    if (hdr.rodataSize) ok &= (f.read(gDataBuf, hdr.rodataSize) == hdr.rodataSize);
    if (hdr.dataSize)   ok &= (f.read(gDataBuf + hdr.rodataSize, hdr.dataSize) == hdr.dataSize);
    f.close();
    if (hdr.bssSize) memset(gDataBuf + hdr.rodataSize + hdr.dataSize, 0, hdr.bssSize);
    if (!ok) { failLoad("truncated section data"); return false; }

    uint8_t *bases[3] = { gTextBuf, gDataBuf, gDataBuf + hdr.rodataSize }; // text, rodata, data
    for (auto &r : relocs) {
        if (r.targetSection > ZAPP_SEC_DATA || r.refSection > ZAPP_SEC_DATA) { failLoad("bad relocation"); return false; }
        uint32_t targetLimit = (r.targetSection == ZAPP_SEC_TEXT) ? hdr.textSize
                              : (r.targetSection == ZAPP_SEC_RODATA) ? hdr.rodataSize : hdr.dataSize;
        if (r.offsetInTarget + 4 > targetLimit) { failLoad("relocation out of range"); return false; }
        uint32_t value = (uint32_t)(bases[r.refSection] + r.addend);
        if (r.targetSection == ZAPP_SEC_TEXT) {
            // Same IRAM 32-bit-only restriction as above - a plain memcpy()
            // here was the second place a stray byte-store into gTextBuf
            // could reboot the device. offsetInTarget is always word-aligned
            // for a genuine pointer-literal relocation, so a direct aligned
            // store is both safe and required.
            if (r.offsetInTarget % 4 != 0) { failLoad("misaligned text relocation"); return false; }
            *(uint32_t *)(bases[ZAPP_SEC_TEXT] + r.offsetInTarget) = value;
        } else {
            memcpy(bases[r.targetSection] + r.offsetInTarget, &value, 4);
        }
    }

    typedef ZyroAppModule (*ZappEntryFn)(const ZyroApi *);
    ZappEntryFn entry = (ZappEntryFn)(gTextBuf + hdr.entryOffset);

    gRunningAppName = appName;
    ZyroAppModule mod = {};
    bool entered = false;
    if (setjmp(gZappJumpBuf) == 0) {
        mod = entry(&gApi);
        entered = true;
    }
    if (!entered || !mod.init || !mod.tick) { failLoad("entry point crashed or incomplete"); return false; }

    gLoaded = mod;
    gAppLoaded = true;
    gZappFaulted = false;
    return true;
}

// ============================== AppModule shim ==============================

void zappSetPending(const char *appFolderName) {
    gPendingAppName = appFolderName ? appFolderName : "";
}

static InputEvent mapKey; // unused, kept for potential future key-repeat needs

static ZyroInput mapInput(const InputResult &in) {
    ZyroInput z{ ZI_NONE, in.ch };
    switch (in.type) {
        case InputEvent::NAV_UP:    z.type = ZI_UP; break;
        case InputEvent::NAV_DOWN:  z.type = ZI_DOWN; break;
        case InputEvent::NAV_LEFT:  z.type = ZI_LEFT; break;
        case InputEvent::NAV_RIGHT: z.type = ZI_RIGHT; break;
        case InputEvent::OK:        z.type = ZI_OK; break;
        case InputEvent::BACK:      z.type = ZI_BACK; break;
        case InputEvent::CHAR:      z.type = ZI_CHAR; break;
        default: z.type = ZI_NONE; break;
    }
    return z;
}

static void zrInit() {
    gExitRequested = false;
    gZappFaulted = false;
    gAppLoaded = false;

    if (gPendingAppName.length() == 0) { failLoad("no app selected"); return; }
    if (!loadZapp(gPendingAppName)) return;

    armCrashGuard(gPendingAppName);
    if (!callGuarded(gLoaded.init)) {
        failLoad(gZappFaultMsg[0] ? gZappFaultMsg : "app crashed during init");
        disarmCrashGuard();
    }
}

static void zrTick() {
    if (!gAppLoaded || gExitRequested) return;
    if (!callGuarded(gLoaded.tick)) {
        disarmCrashGuard();
        showAlert("Error, Review your app on another device", nullptr, rootMenuForceRedraw);
        gExitRequested = true;
        return;
    }
    if (gLoaded.wantsExit && gLoaded.wantsExit()) {
        gExitRequested = true;
    }
}

static void zrHandleInput(const InputResult &in) {
    if (!gAppLoaded || gExitRequested || !gLoaded.handleInput) return;
    ZyroInput z = mapInput(in);
    // handleInput takes an argument, so it can't go through the plain
    // void(*)() signature callGuarded() expects - inline the same guard here.
    if (setjmp(gZappJumpBuf) != 0) {
        gZappFaulted = true;
        disarmCrashGuard();
        showAlert("Error, Review your app on another device", nullptr, rootMenuForceRedraw);
        gExitRequested = true;
        return;
    }
    try {
        gLoaded.handleInput(z);
    } catch (...) {
        gZappFaulted = true;
        disarmCrashGuard();
        showAlert("Error, Review your app on another device", nullptr, rootMenuForceRedraw);
        gExitRequested = true;
    }
}

static void zrOnExit() {
    if (gAppLoaded && !gZappFaulted && gLoaded.onExit) {
        callGuarded(gLoaded.onExit);
    }
    disarmCrashGuard();
    freeAppMemory();
    gAppLoaded = false;
    gLoaded = {};
    gPendingAppName = "";
    gRunningAppName = "";
}

static bool zrWantsExit() { return gExitRequested; }

AppModule zappRunnerAppGet() {
    return { zrInit, zrTick, zrHandleInput, zrOnExit, zrWantsExit };
}
