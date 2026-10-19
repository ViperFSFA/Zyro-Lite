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
#include "touch_scroll.h"

extern AppModule *gActiveApp; // defined in main.cpp. My Apps launches gAppTable[10] the same way root_menu.cpp launches everything else
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

// My Apps (Zyro-SDK user apps)
static std::vector<ZappEntry> myApps;
static int myAppsSel = 0;
static int myAppsScroll = 0;

// Install-from-SD browser: deliberately separate state from the SD File
// Browser above so opening one never disturbs the other's position.
static std::vector<FileEntry> installFiles;
static String installPath = "/";
static int installSel = 0;
static int installScrollTop = 0;
static TouchScroll fileGesture,installGesture,appsGesture;
static bool fileTouch=false,installTouch=false,appsTouch=false;
static uint32_t lastTouchFrame=0;

static void onExit(); // forward decl. MODE_MYAPPS's OK handler below calls this before handing off to the zApp runner

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
    sel = 0; scrollTop = 0;fileGesture.reset();fileTouch=false;
}

// My Apps
static void refreshMyApps() {
    myApps = zappRegistryList();
    if (myAppsSel >= (int)myApps.size()) myAppsSel = (int)myApps.size() - 1;
    if (myAppsSel < 0) myAppsSel = 0;
}

static void drawMyApps() {
    const Theme &t = gSettings.theme();
    uiAppSurface();
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

    if(myAppsSel<myAppsScroll) myAppsScroll=myAppsSel;
    if(myAppsSel>=myAppsScroll+9) myAppsScroll=myAppsSel-8;
    appsGesture.configure(myApps.size(),20,TOPBAR_HEIGHT+26,SCREEN_H-2);
    float offset=appsTouch?appsGesture.position:myAppsScroll*20;
    uiBeginViewport(TOPBAR_HEIGHT+26,SCREEN_H-2);
    int first=int(offset)/20;
    for (int i = first; i < min((int)myApps.size(),first+11); i++) {
        int y = TOPBAR_HEIGHT + 26 + i*20-int(offset);
        bool hi = (i == myAppsSel);
        if (hi) gfx->fillRect(4, y - 2, SCREEN_W - 8, 18, t.accent);
        gfx->setTextColor(hi ? t.accentFg : (myApps[i].blocked ? t.warn : t.fg));
        gfx->setCursor(8, y);
        String tag = myApps[i].blocked ? "[!] " : "    ";
        gfx->print(tag + myApps[i].name);
    }
    uiEndViewport();
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
        // Only show directories and .zApp files anything else installing
        // from here would produce is out of scope, so keep the noise out.
        bool isZapp = name.endsWith(".zApp") || name.endsWith(".ZAPP") || name.endsWith(".zapp");
        if (isDir || isZapp) installFiles.push_back({ name, isDir, (size_t)entry.size() });
        entry = root.openNextFile();
    }
    installSel = 0; installScrollTop = 0;installGesture.reset();installTouch=false;
}

static void drawInstallBrowser() {
    const Theme &t = gSettings.theme();
    uiAppSurface();
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

    installGesture.configure(installFiles.size(),18,TOPBAR_HEIGHT+22,SCREEN_H-2);
    float offset=installTouch?installGesture.position:installScrollTop*18;
    uiBeginViewport(TOPBAR_HEIGHT+20,SCREEN_H-2);
    int first=int(offset)/18;
    for (int i=first; i<min(int(installFiles.size()),first+13); i++) {
        int y = TOPBAR_HEIGHT + 22 + i*18-int(offset);
        bool hi = (i == installSel);
        if (hi) gfx->fillRect(4, y - 2, SCREEN_W - 8, 16, t.accent);
        gfx->setTextColor(hi ? t.accentFg : t.fg);
        gfx->setCursor(8, y);
        String tag = installFiles[i].isDir ? "[DIR] " : "      ";
        gfx->print(tag + installFiles[i].name.substring(0, 20));
    }
    uiEndViewport();
}

static void drawFiles() {
    const Theme &t = gSettings.theme();
    uiAppSurface();

    if (viewingFile) {
        gfx->setTextSize(1);
        gfx->setTextColor(t.accent);
        gfx->setCursor(8, TOPBAR_HEIGHT + 4);
        gfx->print("File Viewer (BACK to exit)");

        gfx->setTextColor(t.fg);
        gfx->setCursor(8, TOPBAR_HEIGHT + 24);
        gfx->setTextWrap(true);
        gfx->print(fileContent.substring(0, 400));
        gfx->setTextWrap(false);
        return;
    }

    gfx->setTextSize(1);
    gfx->setTextColor(t.dim);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print("Path: " + currentPath);

    fileGesture.configure(files.size(),18,TOPBAR_HEIGHT+22,SCREEN_H-2);
    float offset=fileTouch?fileGesture.position:scrollTop*18;
    uiBeginViewport(TOPBAR_HEIGHT+20,SCREEN_H-2);
    int first=int(offset)/18;
    for (int i=first; i<min(int(files.size()),first+13); i++) {
        int y = TOPBAR_HEIGHT + 22 + i*18-int(offset);
        bool hi = (i == sel);
        if (hi) gfx->fillRect(4, y - 2, SCREEN_W - 8, 16, t.accent);
        gfx->setTextColor(hi ? t.accentFg : t.fg);
        gfx->setCursor(8, y);

        String tag = files[i].isDir ? "[DIR] " : "      ";
        gfx->print(tag + files[i].name.substring(0, 20));
    }
    uiEndViewport();
}

static void drawStats() {
    const Theme &t = gSettings.theme();
    uiAppSurface();

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
    gfx->print("Battery    : ~" + String(batteryPercent()) + "% / " + String(batteryMillivolts()) + " mV");

    gfx->setCursor(8, TOPBAR_HEIGHT + 110);
    gfx->print("Uptime     : " + String(millis() / 1000) + " seconds");
}

static void drawDeviceInfo() {
    const Theme &t = gSettings.theme();
    uiAppSurface();

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
    bool changed=false;
    if(currentMode==MODE_FILES && fileTouch && !viewingFile) changed=fileGesture.tick();
    else if(currentMode==MODE_MYAPPS && appsTouch) changed=appsGesture.tick();
    else if(currentMode==MODE_MYAPPS_INSTALL && installTouch) changed=installGesture.tick();
    static bool touchDirty=false;if(changed) touchDirty=true;
    if(touchDirty && millis()-lastTouchFrame>=16) {
        touchDirty=false;lastTouchFrame=millis();
        if(currentMode==MODE_FILES) drawFiles();else if(currentMode==MODE_MYAPPS) drawMyApps();else if(currentMode==MODE_MYAPPS_INSTALL) drawInstallBrowser();
    }
    if (currentMode == MODE_MENU && subSubMenu) {
        subSubMenu->tick();
        return;
    }
}

static void handleInput(const InputResult &in) {
    TouchScroll *gesture=nullptr;bool *touch=nullptr;int rows=0,height=18,offset=0,top=TOPBAR_HEIGHT+22;
    if(currentMode==MODE_FILES && !viewingFile) {gesture=&fileGesture;touch=&fileTouch;rows=files.size();offset=scrollTop*18;}
    else if(currentMode==MODE_MYAPPS) {gesture=&appsGesture;touch=&appsTouch;rows=myApps.size();height=20;top=TOPBAR_HEIGHT+26;offset=myAppsScroll*20;}
    else if(currentMode==MODE_MYAPPS_INSTALL) {gesture=&installGesture;touch=&installTouch;rows=installFiles.size();offset=installScrollTop*18;}
    if(gesture) {
        gesture->configure(rows,height,top,SCREEN_H-2);
        if(in.type==InputEvent::TOUCH_DOWN) {if(!*touch) gesture->reset(offset);*touch=true;}
        if(gesture->handle(in)) {
            if(millis()-lastTouchFrame>=16) {lastTouchFrame=millis();if(currentMode==MODE_FILES) drawFiles();else if(currentMode==MODE_MYAPPS) drawMyApps();else drawInstallBrowser();}return;
        }
        if(in.type==InputEvent::NAV_UP || in.type==InputEvent::NAV_DOWN) *touch=false;
    }
    if (in.type == InputEvent::TOUCH_TAP) {
        if (currentMode == MODE_MENU && subSubMenu) { subSubMenu->handleInput(in); return; }
        if(currentMode==MODE_FILES && !viewingFile) {
            int target=fileTouch?fileGesture.itemAt(in.y):scrollTop+(in.y-TOPBAR_HEIGHT-22)/18;
            if(in.y<TOPBAR_HEIGHT+22 || target<0 || target>=int(files.size())) return;
            sel=target;
        } else if(currentMode==MODE_MYAPPS_INSTALL) {
            int target=installTouch?installGesture.itemAt(in.y):installScrollTop+(in.y-TOPBAR_HEIGHT-22)/18;
            if(in.y<TOPBAR_HEIGHT+22 || target<0 || target>=int(installFiles.size())) return;
            installSel=target;
        } else if(currentMode==MODE_MYAPPS) {
            if(in.y<TOPBAR_HEIGHT+22) { InputResult right; right.type=InputEvent::NAV_RIGHT; handleInput(right); return; }
            int target=appsTouch?appsGesture.itemAt(in.y):myAppsScroll+(in.y-TOPBAR_HEIGHT-26)/20;
            if(in.y<TOPBAR_HEIGHT+26 || target<0 || target>=int(myApps.size())) return;
            myAppsSel=target;
        }
        InputResult ok = in; ok.type = InputEvent::OK; handleInput(ok); return;
    }
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
    // BACK does. has to be checked before the generic BACK case below.
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
            onExit();
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
