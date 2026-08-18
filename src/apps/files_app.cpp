#include "app_api.h"
#include <SD.h>
#include "display.h"
#include "settings.h"
#include "config.h"
#include "audio.h"
#include "battery.h"
#include "menu.h"
#include "zapp_registry.h"
#include "zapp_loader.h"

extern AppModule *gActiveApp; // defined in main.cpp - My Apps launches gAppTable[10] the same way root_menu.cpp launches everything else
extern AppModule gAppTable[];

namespace FilesApp {

enum Mode {
    MODE_MENU = 0,
    MODE_FILES,
    MODE_STATS,
    MODE_DEVICEINFO,
    MODE_MYAPPS,         // Apps > My Apps: list of installed .zApp apps, OK to launch
    MODE_MYAPPS_INSTALL  // Apps > My Apps > Install from SD: file browser filtered for .zApp files
};

static Mode currentMode = MODE_MENU;
static Menu *subSubMenu = nullptr;
static bool exitApp = false;

// SD File Browser state
struct FileEntry { String name; bool isDir; size_t size; };
static std::vector<FileEntry> files;
static String currentPath = "/";
static int sel = 0;
static int scrollTop = 0;
static bool viewingFile = false;
static String fileContent = "";

// --- My Apps (Zyro-SDK user apps) ---
static std::vector<ZappEntry> myApps;
static int myAppsSel = 0;

// Install-from-SD browser: deliberately separate state from the SD File
// Browser above so opening one never disturbs the other's position.
static std::vector<FileEntry> installFiles;
static String installPath = "/";
static int installSel = 0;
static int installScrollTop = 0;

static void onExit(); // forward decl - MODE_MYAPPS's OK handler below calls this before handing off to the zApp runner

static void listDir() {
    files.clear();
    if (currentPath != "/") files.push_back({ "..", true, 0 });

    File root = SD.open(currentPath.c_str());
    if (!root || !root.isDirectory()) return;

    File entry = root.openNextFile();
    while (entry) {
        files.push_back({ String(entry.name()), entry.isDirectory(), (size_t)entry.size() });
        entry = root.openNextFile();
    }
    sel = 0; scrollTop = 0;
}

// --- My Apps ---------------------------------------------------------------
static void refreshMyApps() {
    myApps = zappRegistryList();
    if (myAppsSel >= (int)myApps.size()) myAppsSel = (int)myApps.size() - 1;
    if (myAppsSel < 0) myAppsSel = 0;
}

static void drawMyApps() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);
    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print("My Apps (OK=run, </>=install)");

    if (myApps.empty()) {
        gfx->setTextColor(t.dim);
        gfx->setCursor(8, TOPBAR_HEIGHT + 30);
        gfx->print("No apps installed yet.");
        gfx->setCursor(8, TOPBAR_HEIGHT + 48);
        gfx->print("Press < or > to install one");
        gfx->setCursor(8, TOPBAR_HEIGHT + 64);
        gfx->print("from the SD card.");
        return;
    }

    for (int i = 0; i < (int)myApps.size(); i++) {
        int y = TOPBAR_HEIGHT + 26 + i * 20;
        bool hi = (i == myAppsSel);
        if (hi) gfx->fillRect(4, y - 2, SCREEN_W - 8, 18, t.accent);
        gfx->setTextColor(hi ? t.accentFg : (myApps[i].blocked ? t.warn : t.fg));
        gfx->setCursor(8, y);
        String tag = myApps[i].blocked ? "[!] " : "    ";
        gfx->print(tag + myApps[i].name);
    }
}

static void listInstallDir() {
    installFiles.clear();
    if (installPath != "/") installFiles.push_back({ "..", true, 0 });

    File root = SD.open(installPath.c_str());
    if (!root || !root.isDirectory()) return;

    File entry = root.openNextFile();
    while (entry) {
        String name = String(entry.name());
        bool isDir = entry.isDirectory();
        // Only show directories and .zApp files - anything else installing
        // from here would produce is out of scope, so keep the noise out.
        bool isZapp = name.endsWith(".zApp") || name.endsWith(".ZAPP") || name.endsWith(".zapp");
        if (isDir || isZapp) installFiles.push_back({ name, isDir, (size_t)entry.size() });
        entry = root.openNextFile();
    }
    installSel = 0; installScrollTop = 0;
}

static void drawInstallBrowser() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);
    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print("Install .zApp: " + installPath);

    if (installFiles.empty()) {
        gfx->setTextColor(t.dim);
        gfx->setCursor(8, TOPBAR_HEIGHT + 26);
        gfx->print("No folders or .zApp files here.");
        return;
    }

    int visibleRows = (SCREEN_H - TOPBAR_HEIGHT - 20) / 18;
    for (int row = 0; row < visibleRows; row++) {
        int i = installScrollTop + row;
        if (i >= (int)installFiles.size()) break;
        int y = TOPBAR_HEIGHT + 22 + row * 18;
        bool hi = (i == installSel);
        if (hi) gfx->fillRect(4, y - 2, SCREEN_W - 8, 16, t.accent);
        gfx->setTextColor(hi ? t.accentFg : t.fg);
        gfx->setCursor(8, y);
        String tag = installFiles[i].isDir ? "[DIR] " : "      ";
        gfx->print(tag + installFiles[i].name.substring(0, 20));
    }
}

static void drawFiles() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);

    if (viewingFile) {
        gfx->setTextSize(1);
        gfx->setTextColor(t.accent);
        gfx->setCursor(8, TOPBAR_HEIGHT + 4);
        gfx->print("File Viewer (BACK to exit)");

        gfx->setTextColor(t.fg);
        gfx->setCursor(8, TOPBAR_HEIGHT + 24);
        gfx->print(fileContent.substring(0, 400));
        return;
    }

    gfx->setTextSize(1);
    gfx->setTextColor(t.dim);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print("Path: " + currentPath);

    int visibleRows = (SCREEN_H - TOPBAR_HEIGHT - 20) / 18;
    for (int row = 0; row < visibleRows; row++) {
        int i = scrollTop + row;
        if (i >= (int)files.size()) break;
        int y = TOPBAR_HEIGHT + 22 + row * 18;
        bool hi = (i == sel);
        if (hi) gfx->fillRect(4, y - 2, SCREEN_W - 8, 16, t.accent);
        gfx->setTextColor(hi ? t.accentFg : t.fg);
        gfx->setCursor(8, y);

        String tag = files[i].isDir ? "[DIR] " : "      ";
        gfx->print(tag + files[i].name.substring(0, 20));
    }
}

static void drawStats() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);

    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print("Viper System Performance & Memory");

    gfx->setTextColor(t.fg);
    gfx->setCursor(8, TOPBAR_HEIGHT + 30);
    gfx->print("CPU Freq   : " + String(ESP.getCpuFreqMHz()) + " MHz (Dual-Core)");

    gfx->setCursor(8, TOPBAR_HEIGHT + 50);
    gfx->print("Free Heap  : " + String(ESP.getFreeHeap() / 1024) + " KB / " + String(ESP.getHeapSize() / 1024) + " KB");

    gfx->setCursor(8, TOPBAR_HEIGHT + 70);
    gfx->print("Free PSRAM : " + String(ESP.getFreePsram() / 1024) + " KB");

    gfx->setCursor(8, TOPBAR_HEIGHT + 90);
    gfx->print("Battery    : " + String(batteryPercent()) + "% (" + String(batteryCharging() ? "Charging" : "Discharging") + ")");

    gfx->setCursor(8, TOPBAR_HEIGHT + 110);
    gfx->print("Uptime     : " + String(millis() / 1000) + " seconds");
}

static void drawDeviceInfo() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);

    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print("Device Info");

    gfx->setTextColor(t.fg);
    gfx->setCursor(8, TOPBAR_HEIGHT + 26);
    gfx->print("Firmware   : " FW_NAME " " FW_VERSION);

    gfx->setCursor(8, TOPBAR_HEIGHT + 46);
    gfx->print("Chip       : " + String(ESP.getChipModel()) + " rev" + String(ESP.getChipRevision()));

    gfx->setCursor(8, TOPBAR_HEIGHT + 66);
    gfx->print("Cores      : " + String(ESP.getChipCores()) + " @ " + String(ESP.getCpuFreqMHz()) + " MHz");

    gfx->setCursor(8, TOPBAR_HEIGHT + 86);
    gfx->print("Flash      : " + String(ESP.getFlashChipSize() / (1024 * 1024)) + " MB");

    gfx->setCursor(8, TOPBAR_HEIGHT + 106);
    gfx->print("RF Ver     : " RF_HW_VERSION);

    gfx->setCursor(8, TOPBAR_HEIGHT + 126);
    gfx->setTextColor(t.dim);
    gfx->print("SDK: " + String(ESP.getSdkVersion()));
}

static void init() {
    exitApp = false;
    currentMode = MODE_MENU;

    if (subSubMenu) delete subSubMenu;
    std::vector<MenuItem> items = {
        { "SD File Browser",  ">", [](){ currentMode = MODE_FILES; viewingFile = false; listDir(); drawFiles(); } },
        { "My Apps",          ">", [](){ currentMode = MODE_MYAPPS; refreshMyApps(); drawMyApps(); } },
        { "System Stats",     ">", [](){ currentMode = MODE_STATS; drawStats(); } },
        { "Device Info",      ">", [](){ currentMode = MODE_DEVICEINFO; drawDeviceInfo(); } }
    };
    subSubMenu = new Menu("System Apps", items);
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

    // Install browser's BACK returns one level up (to the My Apps list),
    // not straight out to the System Apps menu like every other screen's
    // BACK does - has to be checked before the generic BACK case below.
    if (currentMode == MODE_MYAPPS_INSTALL) {
        if (in.type == InputEvent::BACK) {
            currentMode = MODE_MYAPPS;
            audioClickBack();
            drawMyApps();
            return;
        }
        int visibleRows = (SCREEN_H - TOPBAR_HEIGHT - 20) / 18;
        if (in.type == InputEvent::NAV_UP) {
            if (installSel > 0) {
                installSel--;
                if (installSel < installScrollTop) installScrollTop = installSel;
                audioClickNav(); drawInstallBrowser();
            }
        } else if (in.type == InputEvent::NAV_DOWN) {
            if (installSel < (int)installFiles.size() - 1) {
                installSel++;
                if (installSel >= installScrollTop + visibleRows) installScrollTop = installSel - visibleRows + 1;
                audioClickNav(); drawInstallBrowser();
            }
        } else if (in.type == InputEvent::OK) {
            if (installFiles.empty()) return;
            audioClickOk();
            FileEntry fe = installFiles[installSel];
            if (fe.name == "..") {
                int p = installPath.lastIndexOf('/', installPath.length() - 2);
                installPath = (p <= 0) ? "/" : installPath.substring(0, p + 1);
                listInstallDir(); drawInstallBrowser();
            } else if (fe.isDir) {
                installPath += fe.name + "/";
                listInstallDir(); drawInstallBrowser();
            } else {
                // Install errors are a PC-side/SDK-build concern to review
                // there, not something to diagnose on-device - a bad file
                // just silently won't appear in the My Apps list below.
                String outName;
                zappRegistryInstall(installPath + fe.name, &outName);
                currentMode = MODE_MYAPPS;
                refreshMyApps();
                drawMyApps();
            }
        }
        return;
    }

    if (in.type == InputEvent::BACK) {
        if (viewingFile) {
            viewingFile = false;
            drawFiles();
            return;
        }
        currentMode = MODE_MENU;
        audioClickBack();
        if (subSubMenu) subSubMenu->draw();
        return;
    }

    if (currentMode == MODE_MYAPPS) {
        if (myApps.empty() && in.type != InputEvent::NAV_LEFT && in.type != InputEvent::NAV_RIGHT) return;
        if (in.type == InputEvent::NAV_UP) {
            if (myAppsSel > 0) { myAppsSel--; audioClickNav(); drawMyApps(); }
        } else if (in.type == InputEvent::NAV_DOWN) {
            if (myAppsSel < (int)myApps.size() - 1) { myAppsSel++; audioClickNav(); drawMyApps(); }
        } else if (in.type == InputEvent::NAV_LEFT || in.type == InputEvent::NAV_RIGHT) {
            currentMode = MODE_MYAPPS_INSTALL;
            installPath = "/";
            listInstallDir();
            drawInstallBrowser();
        } else if (in.type == InputEvent::OK) {
            audioClickOk();
            String name = myApps[myAppsSel].name;
            onExit(); // free this app's own state (subSubMenu etc.) before handing off, same as any other app swap
            zappSetPending(name.c_str());
            gActiveApp = &gAppTable[10];
            gActiveApp->init();
        }
        return;
    }

    if (currentMode == MODE_FILES && !viewingFile) {
        int visibleRows = (SCREEN_H - TOPBAR_HEIGHT - 20) / 18;
        if (in.type == InputEvent::NAV_UP) {
            if (sel > 0) {
                sel--;
                if (sel < scrollTop) scrollTop = sel;
                audioClickNav();
                drawFiles();
            }
        } else if (in.type == InputEvent::NAV_DOWN) {
            if (sel < (int)files.size() - 1) {
                sel++;
                if (sel >= scrollTop + visibleRows) scrollTop = sel - visibleRows + 1;
                audioClickNav();
                drawFiles();
            }
        } else if (in.type == InputEvent::OK) {
            if (files.empty()) return;
            audioClickOk();
            FileEntry fe = files[sel];
            if (fe.name == "..") {
                int p = currentPath.lastIndexOf('/', currentPath.length() - 2);
                currentPath = (p <= 0) ? "/" : currentPath.substring(0, p + 1);
                listDir(); drawFiles();
            } else if (fe.isDir) {
                currentPath += fe.name + "/";
                listDir(); drawFiles();
            } else {
                File f = SD.open((currentPath + fe.name).c_str());
                if (f) {
                    fileContent = "";
                    while (f.available() && fileContent.length() < 350) {
                        fileContent += (char)f.read();
                    }
                    f.close();
                    viewingFile = true;
                    drawFiles();
                }
            }
        }
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

AppModule filesAppGet() {
    return { FilesApp::init, FilesApp::tick, FilesApp::handleInput, FilesApp::onExit, FilesApp::wantsExit };
}
