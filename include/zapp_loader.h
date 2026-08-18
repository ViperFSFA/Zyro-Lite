#pragma once
#include <Arduino.h>
#include "app_api.h"

// ============================================================================
// Zyro-SDK app loader/runner.
//
// A .zApp file is NOT source code and is never compiled on the device - it's
// a small pre-linked binary container (format ZAPP1, see below) produced by
// the Zyro-SDK on a PC. The firmware only ever: reads the file, copies its
// code into an executable RAM buffer, patches a handful of internal
// (same-file) address references, and calls one function pointer. Nothing
// about that process can touch flash, the bootloader, or any other app - a
// bad .zApp can misbehave inside its own sandbox but cannot modify the
// firmware, and every screen it can draw to is clipped below the topbar.
//
// This file also owns the reboot-survives crash guard: if a .zApp locks up
// the CPU or triggers a hardware fault hard enough that the whole device
// resets, that's information ZyroApi::reportError() can't help with (the
// device is already gone). RTC_NOINIT_ATTR memory survives a reset (though
// not a full power-cycle), so a marker set right before the app starts
// running and cleared right after a clean return lets setup() notice "we
// rebooted mid-zApp" and show the same generic error message once at boot
// instead of silently reappearing at the launcher with no explanation.
// ============================================================================

#define ZAPP_MAGIC 0x31504100u // little-endian "ZAP1" as a u32, avoids strcmp on file bytes

// One relocation: patch a 4-byte absolute address at
// (targetSection base + offsetInTarget) to equal
// (refSection base + addend). Only internal (same-binary) references exist
// in a .zApp - see Zyro-SDK/extra_scripts/pack_zapp.py for why no external
// symbol relocations are ever needed.
enum ZappSection : uint8_t { ZAPP_SEC_TEXT = 0, ZAPP_SEC_RODATA = 1, ZAPP_SEC_DATA = 2 };

struct ZappReloc {
    uint8_t targetSection;
    uint8_t refSection;
    uint16_t reserved;
    uint32_t offsetInTarget;
    int32_t addend;
};

struct ZappHeader {
    uint32_t magic;       // ZAPP_MAGIC
    uint32_t apiVersion;  // must equal ZYRO_SDK_API_VERSION
    uint32_t textSize;
    uint32_t rodataSize;
    uint32_t dataSize;
    uint32_t bssSize;
    uint32_t entryOffset; // offset of zapp_entry() within the text buffer
    uint32_t relocCount;  // ZappReloc entries follow immediately after this header
};

// Registers the "My Apps" runner into the shared AppModule table (main.cpp),
// same shape as every built-in app. Call zappSetPending() with the sandboxed
// app folder name (see zapp_registry.h) before launching it, same way
// root_menu.cpp sets gActiveApp - the files_app.cpp "My Apps" screen does
// this instead of root_menu.cpp.
AppModule zappRunnerAppGet();
void zappSetPending(const char *appFolderName);

// Boot-time crash-guard check - call once from setup(), AFTER displayInit()/
// inputInit() but before rootMenuInit(), so the alert overlay can actually
// draw. Shows the standard error screen if the previous session ended mid-
// zApp without a clean exit, then clears the guard. No-ops otherwise.
void zappCheckCrashGuard();
