#pragma once
#include <stdint.h>
#include <stddef.h>

// Zyro-SDK public API. shared between the firmware (Zyro-Lite) and the
// Zyro-SDK app template. this EXACT file must be identical on both sides:
// a .zApp built against a mismatched copy is rejected at load time via
// apiVersion, not by chance layout luck.

// A .zApp NEVER calls firmware/Arduino functions directly (no WiFi.h, no
// gfx->, no SD.h). It only reaches the host through the ZyroApi
// table handed to zapp_entry() at load time. That's what makes it possible
// to sandbox: the app has no linked symbol that could reach outside this
// table, so there is nothing for it to call except what's offered here.

#define ZYRO_SDK_API_VERSION 1u

#ifdef __cplusplus
extern "C" {
#endif

// Input
enum ZyroInputEvent {
    ZI_NONE = 0,
    ZI_UP, ZI_DOWN, ZI_LEFT, ZI_RIGHT,
    ZI_OK, ZI_BACK,
    ZI_CHAR
};

struct ZyroInput {
    ZyroInputEvent type;
    char ch;
};

// Canvas
// Your app never sees the topbar and can never draw over it: (0,0) here is
// always the top-left pixel of the space BELOW the topbar. width/height are
// the usable drawing area. There is no raw framebuffer pointer, no font
// object, nothing that could reach past this rectangle.
struct ZyroCanvas {
    int width;
    int height;
    void (*fillRect)(int x, int y, int w, int h, uint16_t color565);
    void (*drawRect)(int x, int y, int w, int h, uint16_t color565);
    void (*drawPixel)(int x, int y, uint16_t color565);
    void (*drawLine)(int x0, int y0, int x1, int y1, uint16_t color565);
    void (*setCursor)(int x, int y);
    void (*setTextColor)(uint16_t color565);
    void (*print)(const char *text); // draws at the current cursor, built-in font
};

// RGB565 helper. apps build colors with this, same way the firmware uses.
#ifndef ZYRO_RGB565
#define ZYRO_RGB565(r, g, b) ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
#endif

// File
// Sandboxed storage: every path an app gives here is relative to (and
// confined inside) its own /apps/<AppName>/data/ folder on the SD card.
// ".." and absolute paths are rejected by the host before they ever reach
// the SD library. an app cannot read or write anywhere else on the card.
struct ZyroFile {
    bool (*writeAll)(const char *relPath, const uint8_t *data, size_t len);
    int  (*readAll)(const char *relPath, uint8_t *outBuf, size_t maxLen); // returns bytes read, -1 on error
    bool (*exists)(const char *relPath);
    bool (*remove)(const char *relPath);
};

// Wifi
struct ZyroWifi {
    // Blocking scan, fills up to maxNetworks names (32 chars + NUL each).
    bool (*scan)(char names[][33], int maxNetworks, int *outCount);
    bool (*connect)(const char *ssid, const char *pass, uint32_t timeoutMs);
    bool (*isConnected)();
    void (*disconnect)();
};

// BLE
struct ZyroBle {
    bool (*scan)(char names[][33], int maxDevices, int *outCount, uint32_t scanMs);
    bool (*advertiseStart)(const char *name);
    void (*advertiseStop)();
};

// LoRa
struct ZyroLora {
    bool (*available)(); // false if no LoRa radio built/configured on this unit
    bool (*send)(const uint8_t *data, size_t len);
    int  (*receive)(uint8_t *outBuf, size_t maxLen, uint32_t timeoutMs); // bytes received, 0 on timeout, -1 on error
};

// GPIO
// Deliberately index-based, NOT pin-number-based: index i always refers to
// GPIO_CUSTOM_PINS[i] from the firmware's own pins.h allowlist. the same
// list the built-in GPIO app is restricted to. A .zApp can never address a
// pin number directly, so it can never collide with SPI/I2C/display/radio
// wiring, whatever that allowlist happens to contain on a given unit.
struct ZyroGpio {
    int  (*pinCount)();
    bool (*read)(int index);
    void (*write)(int index, bool high);
};

// System
struct ZyroSystem {
    uint32_t (*millis)();
    int      (*batteryPercent)();
    bool     (*batteryCharging)();
    void     (*log)(const char *msg); // Serial only. never shown on-screen
    // Call this to bail out cleanly with the standard on-device error
    // screen ("Error, Review your app on another device") instead of
    // crashing.
    void     (*reportError)(const char *msg);
};

// API
struct ZyroApi {
    uint32_t apiVersion; // == ZYRO_SDK_API_VERSION, checked by the host before entry is ever called     ( - this might change later )
    ZyroCanvas canvas;
    ZyroFile   file;
    ZyroWifi   wifi;
    ZyroBle    ble;
    ZyroLora   lora;
    ZyroGpio   gpio;
    ZyroSystem system;
};

// App module
// Mirrors the firmware's internal AppModule shape 1:1, just aimed at the
// sandboxed ZyroInput instead of the raw one. Every callback is optional
// (nullptr-checked by the host) except init and tick.
struct ZyroAppModule {
    void (*init)();
    void (*tick)();
    void (*handleInput)(const ZyroInput &in);
    void (*onExit)();
    bool (*wantsExit)();
};

// Every .zApp must define exactly this, with C linkage (extern "C") so the
// packer can find it by an unmangled name:

//   extern "C" ZyroAppModule zapp_entry(const ZyroApi *api);

// Called once at launch. Stash the api pointer (a plain static in your .cpp
// is fine and expected. see the SDK template) and return your callback
// table. The host calls init() right after.

#ifdef __cplusplus
}
#endif
