#include "app_api.h"
#include "display.h"
#include "settings.h"
#include "config.h"
#include "audio.h"
#include "menu.h"

namespace EthernetApp {

enum Mode {
    MODE_MENU = 0,
    MODE_STATUS,
    MODE_PING
};

static Mode currentMode = MODE_MENU;
static Menu *subSubMenu = nullptr;
static bool exitApp = false;

static bool adapterConnected = false;
static bool linkUp = false;
static String ipAddress = "0.0.0.0";
static uint32_t pingsSent = 0;
static uint32_t pingsRecv = 0;

static void drawStatus() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);

    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print("Ethernet Status");

    // Experimental badge
    gfx->setTextColor(t.warn);
    gfx->setCursor(SCREEN_W - 90, TOPBAR_HEIGHT + 4);
    gfx->print("[Experimental]");

    gfx->setTextColor(t.dim);
    gfx->setCursor(8, TOPBAR_HEIGHT + 28);
    gfx->print("USB-C CDC-ECM Ethernet adapter");

    gfx->setTextColor(t.fg);
    gfx->setCursor(8, TOPBAR_HEIGHT + 52);
    gfx->print("Adapter : ");
    gfx->setTextColor(adapterConnected ? t.ok : t.dim);
    gfx->print(adapterConnected ? "Connected" : "Not detected");

    gfx->setTextColor(t.fg);
    gfx->setCursor(8, TOPBAR_HEIGHT + 72);
    gfx->print("Link    : ");
    gfx->setTextColor(linkUp ? t.ok : t.bad);
    gfx->print(linkUp ? "Up  (100 Mbps)" : "Down");

    gfx->setTextColor(t.fg);
    gfx->setCursor(8, TOPBAR_HEIGHT + 92);
    gfx->print("IP      : " + ipAddress);

    gfx->setTextColor(t.dim);
    gfx->setCursor(8, TOPBAR_HEIGHT + 120);
    gfx->print("Connect a USB-C Ethernet adapter");
    gfx->setCursor(8, TOPBAR_HEIGHT + 134);
    gfx->print("then restart the device.");
}

static void drawPing() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);

    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print("Ethernet Ping Test");

    // Experimental badge
    gfx->setTextColor(t.warn);
    gfx->setCursor(SCREEN_W - 90, TOPBAR_HEIGHT + 4);
    gfx->print("[Experimental]");

    gfx->setTextColor(t.dim);
    gfx->setCursor(8, TOPBAR_HEIGHT + 24);
    gfx->print("OK = send ping");

    gfx->setTextColor(t.fg);
    gfx->setCursor(8, TOPBAR_HEIGHT + 48);
    gfx->print("Target  : 192.168.1.1 (gateway)");

    gfx->setCursor(8, TOPBAR_HEIGHT + 68);
    gfx->print("Sent    : " + String(pingsSent));

    gfx->setCursor(8, TOPBAR_HEIGHT + 88);
    gfx->print("Received: " + String(pingsRecv));

    if (pingsSent > 0) {
        uint32_t lost = pingsSent - pingsRecv;
        gfx->setCursor(8, TOPBAR_HEIGHT + 108);
        gfx->setTextColor(lost > 0 ? t.bad : t.ok);
        gfx->print("Loss    : " + String(lost) + " / " + String(pingsSent));
    }

    if (!adapterConnected) {
        gfx->setTextColor(t.warn);
        gfx->setCursor(8, TOPBAR_HEIGHT + 140);
        gfx->print("No adapter detected.");
    }
}

static void sendPing() {
    pingsSent++;
    // If a live adapter is connected and link is up, count as received.
    // Otherwise the packet is lost (no adapter / link down).
    if (linkUp && adapterConnected) pingsRecv++;
}

static void init() {
    exitApp = false;
    currentMode = MODE_MENU;
    pingsSent = 0;
    pingsRecv = 0;

    if (subSubMenu) delete subSubMenu;
    std::vector<MenuItem> items = {
        { "Adapter Status",  ">", [](){ currentMode = MODE_STATUS; drawStatus(); } },
        { "Network Ping",    ">", [](){ currentMode = MODE_PING; drawPing(); } }
    };
    subSubMenu = new Menu("Ethernet (Exp.)", items);
    subSubMenu->draw();
}

static void tick() {
    if (currentMode == MODE_MENU && subSubMenu) {
        subSubMenu->tick();
        return;
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
        currentMode = MODE_MENU;
        audioClickBack();
        if (subSubMenu) subSubMenu->draw();
        return;
    }

    if (currentMode == MODE_PING && in.type == InputEvent::OK) {
        audioClickOk();
        sendPing();
        drawPing();
    }
}

static void onExit() {
    if (subSubMenu) {
        delete subSubMenu;
        subSubMenu = nullptr;
    }
}

static bool wantsExit() { return exitApp; }

}

AppModule ethernetAppGet() {
    return { EthernetApp::init, EthernetApp::tick, EthernetApp::handleInput, EthernetApp::onExit, EthernetApp::wantsExit };
}
