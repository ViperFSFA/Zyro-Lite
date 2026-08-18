// Zyro-SDK example app. copy this file, rename it, and start editing.

// Build it with:
//     python Zyro-SDK/tools/pack_zapp.py YourAppName.cpp

// This produces build/YourAppName/YourAppName.zApp, copy that file
// anywhere on the SD card, then on the device:
//     Apps > My Apps > Install from SD > pick the .zApp file

// Rules that keep your app safe to run (enforced by the SDK/firmware, not
// just convention):
//   - You can ONLY call functions reachable through the ZyroApi table handed
//     to zapp_entry(). There is no WiFi.h, no SD.h, no gfx pointer, nothing
//     else to link against, the compiler will simply fail to build anything
//     that reaches outside this table.

//   - canvas coordinates are already relative to the space below the topbar.
//     You cannot draw over the topbar. don't bother computing an offset for it.

//   - file I/O is confined to your own app's private data/ folder. Relative
//     paths only.

//   - if something goes wrong, call api->system.reportError("short reason")
//     instead of crashing. the user sees the standard error screen, and it's
//     enough of a fault container that a bug in your app can't reboot loop
//     or freeze the whole device.

#include "zyro_sdk_api.h"
#include "zyro_runtime.h"

// Stash the API pointer once at launch, this is the ONLY global state you
// need to reach every host function from anywhere in your app.
static const ZyroApi *api = nullptr;

// your app's own state
static int counter = 0;
static bool exitRequested = false;
static char lineBuf[48];

// Lifecycle
static void appInit() {
    counter = 0;
    exitRequested = false;
    api->system.log("Example app started");
}

static void appTick() {
    // Runs every frame. Keep this fast, don't block for long stretches or the whole UI (including the topbar clock/battery) stalls with you.
    api->canvas.fillRect(0, 0, api->canvas.width, api->canvas.height, ZYRO_RGB565(10, 10, 20));

    api->canvas.setCursor(8, 8);
    api->canvas.setTextColor(ZYRO_RGB565(255, 255, 255));
    api->canvas.print("Zyro-SDK example app");

    api->canvas.setCursor(8, 30);
    zyro_strcat(lineBuf, sizeof(lineBuf), "");
    lineBuf[0] = 0;
    char numBuf[12];
    zyro_itoa(counter, numBuf, sizeof(numBuf));
    zyro_strcat(lineBuf, sizeof(lineBuf), "Ticks: ");
    zyro_strcat(lineBuf, sizeof(lineBuf), numBuf);
    api->canvas.print(lineBuf);

    api->canvas.setCursor(8, 50);
    char battLine[32] = "";
    zyro_itoa(api->system.batteryPercent(), numBuf, sizeof(numBuf));
    zyro_strcat(battLine, sizeof(battLine), "Battery: ");
    zyro_strcat(battLine, sizeof(battLine), numBuf);
    zyro_strcat(battLine, sizeof(battLine), "%");
    api->canvas.print(battLine);

    api->canvas.drawRect(4, 4, api->canvas.width - 8, api->canvas.height - 8, ZYRO_RGB565(80, 80, 120));

    counter++;
}

static void appHandleInput(const ZyroInput &in) {
    if (in.type == ZI_BACK) {
        exitRequested = true;
    }
    // ZI_UP / ZI_DOWN / ZI_LEFT / ZI_RIGHT / ZI_OK / ZI_CHAR (in.ch) are also
    // available. see zyro_sdk_api.h's ZyroInputEvent.
}

static void appOnExit() {
    api->system.log("Example app exiting cleanly");
}

static bool appWantsExit() {
    return exitRequested;
}

// Entrypoint
// Every .zApp must define exactly this, unmangled (extern "C"), so
// pack_zapp.py can locate it by name.
extern "C" ZyroAppModule zapp_entry(const ZyroApi *hostApi) {
    api = hostApi;
    ZyroAppModule mod;
    mod.init = appInit;
    mod.tick = appTick;
    mod.handleInput = appHandleInput;
    mod.onExit = appOnExit;
    mod.wantsExit = appWantsExit;
    return mod;
}
