#include "menu.h"
#include "app_api.h"
#include "config.h"

// Animated bitmap icons for menu rows (64x64, 1bpp, 28 frames each).
#include "gears_64_64_28f.h"       // Settings
#include "compass_64_64_28f.h"     // GPS
#include "connect_64_64_28f.h"     // Ethernet
#include "qr_code_64_64_28f.h"     // LoRa
#include "wifi_search_64_64_28f.h" // WiFi
#include "activity_64_64_28f.h"    // RF
#include "money_64_64_28f.h"       // Games
#include "add_folder_64_64_28f.h"  // Apps (file browser)
#include "ble_64_64_28f.h"         // BLE (converted from a static bitmap - all frames identical)

extern AppModule *gActiveApp; // defined in main.cpp
extern AppModule gAppTable[]; // defined in main.cpp, indexed same order as items below

static Menu *root = nullptr;

static void launch(int idx);

void rootMenuInit() {
    // BUGFIX: previously this allocated a new Menu every time the launcher
    // was re-entered (i.e. every time an app exited) without ever freeing
    // the old one, leaking heap on every app exit until the device eventually
    // ran out of memory and crashed. Free the previous instance first.
    if (root) {
        delete root;
        root = nullptr;
    }

    // Two-letter monograms are the fallback "icon" for rows that don't have a
    // bitmap animation wired in yet. Every row currently has one.
    // Rows with a bitmap icon (last two args) render that instead: static
    // frame 0 normally, animating only while that row is the highlighted
    // selection.
    std::vector<MenuItem> items = {
        { "WiFi",      "WF", [](){ launch(1); }, wifi_search_64_64_28f_frames, ICON_FRAME_COUNT },
        { "BLE",       "BT", [](){ launch(2); }, ble_64_64_28f_frames, ICON_FRAME_COUNT },
        { "LoRa",      "LR", [](){ launch(3); }, qr_code_64_64_28f_frames, ICON_FRAME_COUNT },
        { "RF",        "RF", [](){ launch(4); }, activity_64_64_28f_frames, ICON_FRAME_COUNT },
        { "GPS",       "GP", [](){ launch(5); }, compass_64_64_28f_frames, ICON_FRAME_COUNT },
        { "Ethernet",  "EN", [](){ launch(6); }, connect_64_64_28f_frames, ICON_FRAME_COUNT },
        { "Games",     "GM", [](){ launch(7); }, money_64_64_28f_frames, ICON_FRAME_COUNT },
        { "Apps",      "AP", [](){ launch(8); }, add_folder_64_64_28f_frames, ICON_FRAME_COUNT },
        { "Settings",  "ST", [](){ launch(0); }, gears_64_64_28f_frames, ICON_FRAME_COUNT },
    };
    root = new Menu("Zyro-Lite", items);
    root->draw();
}

static void launch(int idx) {
    gActiveApp = &gAppTable[idx];
    gActiveApp->init();
}

void rootMenuTick() {
    if (root) root->tick();
}

void rootMenuHandleInput(const InputResult &in) {
    if (root) root->handleInput(in);
}

void rootMenuForceRedraw() {
    if (root) root->forceRedraw();
}
