#include "app_api.h"
#include <WiFi.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include <algorithm>
#include "display.h"
#include "settings.h"
#include "config.h"
#include "audio.h"
#include "menu.h"
#include "overlay.h"

namespace WifiApp {

enum Mode {
    MODE_MENU = 0,
    MODE_SCANNER,
    MODE_SPECTRUM,
    MODE_MONITOR,
    MODE_PACKETS,
    MODE_BEACON_SPAM,
    MODE_CONNECT_LIST,
    MODE_CONNECT_FORM,
    MODE_CONNECT_RESULT
};

static Mode currentMode = MODE_MENU;
static Menu *subSubMenu = nullptr;

// Scanner state
static int netCount = 0;
static int scrollTop = 0;
static int sel = 0;
static bool scanning = false;
static bool scanPending = false; // true while an async WiFi.scanNetworks() is in flight

// Networks sorted strongest-first for the connect list (built from the raw
// scan results once they're in - WiFi.scanNetworks() doesn't guarantee any
// particular order). sortedOrder[i] is a real index into the WiFi.SSID()/
// WiFi.RSSI()/etc arrays.
static std::vector<int> sortedOrder;

// Spectrum state
static int channelCounts[14] = {0};
static int channelMaxRssi[14] = {-100};

// Signal Monitor state - this now tracks the RSSI of whatever network we're
// actually connected to (WiFi.RSSI(), no args), not a scan result, so it
// reflects something real instead of an arbitrary scanned AP.
static int signalHistory[40] = {0};
static int historyIdx = 0;
static uint32_t lastMonSample = 0;

// Packet Monitor state - counts real 802.11 frames via the radio's
// promiscuous mode, independent of whether we're connected to anything.
static volatile uint32_t packetCounter = 0;
static int packetHistory[40] = {0};
static int packetHistIdx = 0;
static uint32_t lastPacketSampleMs = 0;
static uint8_t snifferChannel = 1;
static uint32_t lastChannelHopMs = 0;
static bool promiscuousOn = false;
static uint32_t packetPeak = 1;

// Connect-to-network state
static String connectSsid = "";
static bool connectOpen = false;
static String passwordBuf = "";
static bool showPassword = false;
static int formSel = 0;        // 0=password, 1=show password, 2=connect
static bool formEditing = false;
static bool connecting = false;
static uint32_t connectStartMs = 0;
static const uint32_t CONNECT_TIMEOUT_MS = 15000;
static int resultSel = 0;      // 0=Disconnect, 1=Scan for other networks, on the status screen

// Forward decls: pollScan() (below) needs to redraw whichever screen asked
// for the scan once results are ready, but those draw functions aren't
// defined until further down this file.
static void drawScanner();
static void drawSpectrum();
static void drawConnectList();
static void drawConnectForm();
static void drawConnectResult();

static void doScan() {
    if (scanPending) return;
    scanning = true;
    scanPending = true;
    // Used to force WiFi.disconnect() here before scanning, which dropped an
    // active connection every time you opened the scanner, spectrum view or
    // connect list - scanNetworks() doesn't need that, it works fine
    // alongside an existing connection. Only bring the radio up if it was
    // fully off.
    if (WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_STA);
    WiFi.scanNetworks(true /*async*/, true /*show_hidden*/);
    showLoadingOverlay("Scanning Wi-Fi...");
}

// Called every tick() while a scan is in flight. Non-blocking: returns
// immediately if the scan isn't done yet (the loading overlay keeps spinning
// via loadingOverlayTick(), called globally from main.cpp's loop()).
static void pollScan() {
    if (!scanPending) return;

    int16_t result = WiFi.scanComplete();
    if (result == WIFI_SCAN_RUNNING) return;

    scanPending = false;
    scanning = false;
    hideLoadingOverlay();

    netCount = (result < 0) ? 0 : result;

    // Reset spectrum stats
    for (int i = 0; i < 14; i++) {
        channelCounts[i] = 0;
        channelMaxRssi[i] = -100;
    }
    for (int i = 0; i < netCount; i++) {
        int ch = WiFi.channel(i);
        if (ch >= 1 && ch <= 13) {
            channelCounts[ch]++;
            int rssi = WiFi.RSSI(i);
            if (rssi > channelMaxRssi[ch]) channelMaxRssi[ch] = rssi;
        }
    }

    // Strongest signal first for the connect list.
    sortedOrder.clear();
    for (int i = 0; i < netCount; i++) sortedOrder.push_back(i);
    std::sort(sortedOrder.begin(), sortedOrder.end(), [](int a, int b) {
        return WiFi.RSSI(a) > WiFi.RSSI(b);
    });

    sel = 0;
    scrollTop = 0;

    // Redraw whichever screen actually asked for this scan, now that results
    // are in (the loading overlay was covering it a moment ago).
    if (currentMode == MODE_SCANNER) drawScanner();
    else if (currentMode == MODE_SPECTRUM) drawSpectrum();
    else if (currentMode == MODE_CONNECT_LIST) drawConnectList();
}

static void onConnectFailDismissed() {
    if (currentMode == MODE_CONNECT_FORM) drawConnectForm();
}

static void startConnect(const String &password) {
    connecting = true;
    connectStartMs = millis();
    WiFi.mode(WIFI_STA);
    if (password.length() > 0) WiFi.begin(connectSsid.c_str(), password.c_str());
    else WiFi.begin(connectSsid.c_str());
    String label = "Connecting to " + connectSsid.substring(0, 20);
    showLoadingOverlay(label.c_str());
}

static void pollConnect() {
    if (!connecting) return;

    if (WiFi.status() == WL_CONNECTED) {
        connecting = false;
        hideLoadingOverlay();
        currentMode = MODE_CONNECT_RESULT;
        resultSel = 0;
        drawConnectResult();
        return;
    }

    if (millis() - connectStartMs > CONNECT_TIMEOUT_MS) {
        connecting = false;
        hideLoadingOverlay();
        WiFi.disconnect();
        showAlert("Wi-Fi Connect Failed", nullptr, onConnectFailDismissed);
    }
}

// Rough signal-quality label - easier to scan at a glance than raw dBm
// alone, without needing custom bar glyphs the font might not have.
static const char *signalWord(int rssi) {
    if (rssi >= -55) return "Strong";
    if (rssi >= -70) return "OK";
    return "Weak";
}

static void drawScanner() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);
    gfx->setTextSize(1);
    gfx->setTextColor(t.dim);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print(scanning ? "Scanning Wi-Fi..." : String(netCount) + " APs  (SPACE=rescan, BACK=menu)");

    int visibleRows = (SCREEN_H - TOPBAR_HEIGHT - 20) / 18;
    for (int row = 0; row < visibleRows; row++) {
        int i = scrollTop + row;
        if (i >= netCount) break;
        int y = TOPBAR_HEIGHT + 22 + row * 18;
        bool hi = (i == sel);
        if (hi) gfx->fillRect(4, y - 2, SCREEN_W - 8, 16, t.accent);
        gfx->setTextColor(hi ? t.accentFg : t.fg);
        gfx->setCursor(8, y);

        String enc = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "OPEN" : "SEC";
        gfx->print(String(WiFi.SSID(i)).substring(0, 15) + " ch" + String(WiFi.channel(i)) +
                    " " + String(WiFi.RSSI(i)) + "dBm " + enc);
    }
}

static void drawConnectList() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);
    gfx->setTextSize(1);
    gfx->setTextColor(t.dim);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print(scanning ? "Scanning Wi-Fi..." : String(netCount) + " networks, strongest first  (OK=connect)");

    int visibleRows = (SCREEN_H - TOPBAR_HEIGHT - 20) / 18;
    for (int row = 0; row < visibleRows; row++) {
        int listPos = scrollTop + row;
        if (listPos >= netCount) break;
        int i = sortedOrder[listPos];
        int y = TOPBAR_HEIGHT + 22 + row * 18;
        bool hi = (listPos == sel);
        if (hi) gfx->fillRect(4, y - 2, SCREEN_W - 8, 16, t.accent);
        gfx->setTextColor(hi ? t.accentFg : t.fg);
        gfx->setCursor(8, y);

        String enc = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "OPEN" : "SEC";
        int rssi = WiFi.RSSI(i);
        gfx->print(String(WiFi.SSID(i)).substring(0, 14) + " " + signalWord(rssi) +
                    " (" + String(rssi) + "dBm) " + enc);
    }
}

static void drawConnectForm() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);
    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print("Connect: " + connectSsid.substring(0, 24));

    String mask = "";
    for (size_t i = 0; i < passwordBuf.length(); i++) mask += "*";
    String pwValue = showPassword ? passwordBuf : mask;
    if (pwValue.length() == 0) pwValue = "(empty)";

    auto row = [&](int i, const String &label, const String &value, int y) {
        bool hi = (formSel == i);
        if (hi) {
            gfx->fillRoundRect(6, y, SCREEN_W - 12, 22, 6, t.accent);
            gfx->setTextColor(t.accentFg);
        } else {
            gfx->setTextColor(t.fg);
        }
        gfx->setCursor(12, y + 6);
        gfx->print(label);
        gfx->setCursor(SCREEN_W - 14 - value.length() * 6, y + 6);
        gfx->print(value);
    };

    int y0 = TOPBAR_HEIGHT + 30;
    row(0, formEditing ? "Password (typing)" : "Password", pwValue, y0);
    row(1, "Show password", showPassword ? "On" : "Off", y0 + 26);
    row(2, "Connect", "", y0 + 52);

    gfx->setTextColor(t.dim);
    gfx->setCursor(8, y0 + 80);
    if (formEditing) gfx->print("Type on keyboard, ENTER=done, BACK=erase");
    else gfx->print("OK on Password to type it, OK on Connect to join");
}

static void drawConnectResult() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);
    gfx->setTextSize(1);
    gfx->setTextColor(t.ok);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print("Connected to " + connectSsid.substring(0, 20));

    gfx->setTextColor(t.fg);
    gfx->setCursor(8, TOPBAR_HEIGHT + 26);
    gfx->print("IP: " + WiFi.localIP().toString());
    gfx->setCursor(8, TOPBAR_HEIGHT + 42);
    gfx->print("Signal: " + String(WiFi.RSSI()) + " dBm (" + signalWord(WiFi.RSSI()) + ")");

    auto row = [&](int i, const String &label, int y) {
        bool hi = (resultSel == i);
        if (hi) {
            gfx->fillRoundRect(6, y, SCREEN_W - 12, 22, 6, t.accent);
            gfx->setTextColor(t.accentFg);
        } else {
            gfx->setTextColor(t.fg);
        }
        gfx->setCursor(12, y + 6);
        gfx->print(label);
    };

    int y0 = TOPBAR_HEIGHT + 70;
    row(0, "Disconnect", y0);
    row(1, "Scan for other networks", y0 + 26);
}

static void drawSpectrum() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);
    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print("2.4GHz Spectrum Analyzer (SPACE=rescan)");

    int startY = SCREEN_H - 30;
    int maxBarH = SCREEN_H - TOPBAR_HEIGHT - 65;
    int barW = 18;
    int gap = 4;
    int startX = 14;

    for (int ch = 1; ch <= 13; ch++) {
        int x = startX + (ch - 1) * (barW + gap);
        int cnt = channelCounts[ch];
        int h = map(constrain(cnt, 0, 10), 0, 10, 4, maxBarH);

        gfx->fillRect(x, startY - h, barW, h, t.accent);
        gfx->drawRect(x, startY - maxBarH, barW, maxBarH, t.dim);

        gfx->setTextColor(t.fg);
        gfx->setCursor(x + 2, startY + 4);
        gfx->print(ch);

        gfx->setTextColor(t.dim);
        gfx->setCursor(x, startY - h - 10);
        gfx->print(cnt);
    }
}

static void drawMonitor() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);
    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print("Signal Monitor - " + WiFi.SSID());

    int graphX = 20;
    int graphY = TOPBAR_HEIGHT + 25;
    int graphW = 280;
    int graphH = 140;

    gfx->drawRect(graphX, graphY, graphW, graphH, t.dim);

    for (int i = 0; i < 39; i++) {
        int idx1 = (historyIdx + i) % 40;
        int idx2 = (historyIdx + i + 1) % 40;
        int val1 = signalHistory[idx1];
        int val2 = signalHistory[idx2];

        int y1 = graphY + graphH - map(constrain(val1, -100, -30), -100, -30, 0, graphH);
        int y2 = graphY + graphH - map(constrain(val2, -100, -30), -100, -30, 0, graphH);

        int x1 = graphX + (i * graphW / 39);
        int x2 = graphX + ((i + 1) * graphW / 39);

        gfx->drawLine(x1, y1, x2, y2, t.ok);
    }

    gfx->setTextColor(t.fg);
    gfx->setCursor(graphX, graphY + graphH + 8);
    gfx->print("Last RSSI: " + String(signalHistory[(historyIdx + 39) % 40]) + " dBm");
}

static void drawNeedsConnection(const char *title) {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);
    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print(title);

    gfx->setTextColor(t.warn);
    gfx->setCursor(8, TOPBAR_HEIGHT + 40);
    gfx->print("Connect to a network first");

    gfx->setTextColor(t.dim);
    gfx->setCursor(8, TOPBAR_HEIGHT + 60);
    gfx->print("Use \"Connect to Network\" from the");
    gfx->setCursor(8, TOPBAR_HEIGHT + 72);
    gfx->print("Wi-Fi Tools menu, then come back here.");
}

static void drawPacketMonitor() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);
    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    if (WiFi.status() == WL_CONNECTED) {
        gfx->print("Packet Monitor - ch " + String(WiFi.channel()));
    } else {
        gfx->print("Packet Monitor - hopping ch " + String(snifferChannel));
    }

    int graphX = 20;
    int graphY = TOPBAR_HEIGHT + 25;
    int graphW = 280;
    int graphH = 140;

    gfx->drawRect(graphX, graphY, graphW, graphH, t.dim);

    for (int i = 0; i < 39; i++) {
        int idx1 = (packetHistIdx + i) % 40;
        int idx2 = (packetHistIdx + i + 1) % 40;
        int val1 = packetHistory[idx1];
        int val2 = packetHistory[idx2];

        int y1 = graphY + graphH - map(constrain(val1, 0, (int)packetPeak), 0, (int)packetPeak, 0, graphH);
        int y2 = graphY + graphH - map(constrain(val2, 0, (int)packetPeak), 0, (int)packetPeak, 0, graphH);

        int x1 = graphX + (i * graphW / 39);
        int x2 = graphX + ((i + 1) * graphW / 39);

        gfx->drawLine(x1, y1, x2, y2, t.accent);
    }

    gfx->setTextColor(t.fg);
    gfx->setCursor(graphX, graphY + graphH + 8);
    gfx->print("Last: " + String(packetHistory[(packetHistIdx + 39) % 40]) + " pkts/250ms");
}

static void IRAM_ATTR onPromiscuousPacket(void *buf, wifi_promiscuous_pkt_type_t type) {
    packetCounter++;
}

static void startPacketMonitor() {
    if (promiscuousOn) return;
    if (WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_STA);
    esp_wifi_set_promiscuous_rx_cb(&onPromiscuousPacket);
    esp_wifi_set_promiscuous(true);
    promiscuousOn = true;
    packetCounter = 0;
    packetPeak = 1;
    for (int i = 0; i < 40; i++) packetHistory[i] = 0;
    packetHistIdx = 0;
    lastPacketSampleMs = millis();
    lastChannelHopMs = millis();
}

static void stopPacketMonitor() {
    if (!promiscuousOn) return;
    esp_wifi_set_promiscuous(false);
    promiscuousOn = false;
}

// --- Beacon Spammer ---
// Broadcasts spoofed 802.11 beacon frames with randomized SSIDs and source
// MACs over raw WiFi TX - shows up as noise/fake APs to anything scanning
// nearby, same idea as the classic ESP32 "beacon spam" tools. No deauth, no
// client targeting, nothing aimed at an existing network - it's broadcast
// frames only. Runs purely on a timer while this screen is open and stops
// the moment you leave it (see stopBeaconSpam(), called from BACK and
// onExit() below) so it never keeps running in the background.
static bool beaconSpamActive = false;
static uint32_t beaconSpamCount = 0;
static uint32_t lastBeaconSpamMs = 0;
static uint32_t lastBeaconSpamDrawMs = 0;
static const uint32_t BEACON_SPAM_INTERVAL_MS = 100;

// Minimal beacon frame: 802.11 header + fixed params + SSID tag. Rates/DS
// tags get appended per-packet after the (variable-length) SSID.
static const uint8_t BEACON_TEMPLATE[24 + 12] = {
    0x80, 0x00,                                     // frame control: mgmt/beacon
    0x00, 0x00,                                     // duration
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,              // destination: broadcast
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,              // source addr (filled per-packet)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,              // BSSID (filled per-packet)
    0x00, 0x00,                                      // seq-ctl
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // timestamp
    0x64, 0x00,                                      // beacon interval (100 TU)
    0x01, 0x04,                                      // capability info (ESS, short preamble)
};

static void sendRandomBeacon() {
    uint8_t pkt[128];
    memcpy(pkt, BEACON_TEMPLATE, sizeof(BEACON_TEMPLATE));

    // Random source MAC/BSSID. Locally-administered bit (0x02) set so this
    // never collides with a real device's assigned vendor OUI.
    uint8_t mac[6];
    mac[0] = 0x02;
    for (int i = 1; i < 6; i++) mac[i] = (uint8_t)esp_random();
    memcpy(&pkt[10], mac, 6);
    memcpy(&pkt[16], mac, 6);

    static const char *words[] = { "Free WiFi", "NETGEAR", "Home", "Guest",
                                    "Office", "Linksys", "TP-Link", "Xfinity" };
    String ssid = String(words[esp_random() % (sizeof(words) / sizeof(words[0]))]) +
                  "_" + String(esp_random() % 10000);
    if (ssid.length() > 32) ssid = ssid.substring(0, 32);

    int offset = sizeof(BEACON_TEMPLATE);
    pkt[offset++] = 0x00;               // SSID tag id
    pkt[offset++] = (uint8_t)ssid.length();
    memcpy(&pkt[offset], ssid.c_str(), ssid.length());
    offset += ssid.length();

    // Supported rates tag
    static const uint8_t rates[] = { 0x01, 0x08, 0x82, 0x84, 0x8b, 0x96, 0x24, 0x30, 0x48, 0x6c };
    memcpy(&pkt[offset], rates, sizeof(rates));
    offset += sizeof(rates);

    // DS Parameter Set tag - random channel 1-11, just for variety.
    pkt[offset++] = 0x03;
    pkt[offset++] = 0x01;
    pkt[offset++] = (uint8_t)((esp_random() % 11) + 1);

    esp_wifi_80211_tx(WIFI_IF_STA, pkt, offset, false);
    beaconSpamCount++;
}

static void startBeaconSpam() {
    if (WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_STA);
    beaconSpamActive = true;
    beaconSpamCount = 0;
    lastBeaconSpamMs = 0;
    lastBeaconSpamDrawMs = 0;
}

static void stopBeaconSpam() {
    beaconSpamActive = false;
}

static void drawBeaconSpam() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);
    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print("Beacon Spammer");

    gfx->setTextColor(t.fg);
    gfx->setCursor(8, TOPBAR_HEIGHT + 30);
    gfx->print("Beacons sent: " + String(beaconSpamCount));

    gfx->setTextColor(beaconSpamActive ? t.ok : t.dim);
    gfx->setCursor(8, TOPBAR_HEIGHT + 54);
    gfx->print(beaconSpamActive ? "Broadcasting random SSIDs..." : "Stopped");

    gfx->setTextColor(t.dim);
    gfx->setCursor(8, TOPBAR_HEIGHT + 80);
    gfx->print("Stops automatically on exit (BACK)");
}

static bool exitApp = false;

static void init() {
    exitApp = false;
    currentMode = MODE_MENU;

    if (subSubMenu) delete subSubMenu;
    std::vector<MenuItem> items = {
        { "AP Scanner",         ">", [](){ currentMode = MODE_SCANNER; doScan(); } },
        { "2.4GHz Spectrum",    ">", [](){
              if (WiFi.status() != WL_CONNECTED) { currentMode = MODE_SPECTRUM; drawNeedsConnection("2.4GHz Spectrum Analyzer"); }
              else { currentMode = MODE_SPECTRUM; doScan(); }
          } },
        { "Signal Monitor",     ">", [](){
              if (WiFi.status() != WL_CONNECTED) { currentMode = MODE_MONITOR; drawNeedsConnection("Signal Monitor"); }
              else { currentMode = MODE_MONITOR; lastMonSample = 0; drawMonitor(); }
          } },
        { "Packet Monitor",     ">", [](){ currentMode = MODE_PACKETS; startPacketMonitor(); drawPacketMonitor(); } },
        { "Beacon Spammer",     ">", [](){ currentMode = MODE_BEACON_SPAM; startBeaconSpam(); drawBeaconSpam(); } },
        { "Connect to Network", ">", [](){
              if (WiFi.status() == WL_CONNECTED) { currentMode = MODE_CONNECT_RESULT; resultSel = 0; drawConnectResult(); }
              else { currentMode = MODE_CONNECT_LIST; doScan(); }
          } }
    };
    subSubMenu = new Menu("Wi-Fi Tools", items);
    subSubMenu->draw();
}

static void tick() {
    pollScan();
    pollConnect();

    if (currentMode == MODE_MENU && subSubMenu) {
        subSubMenu->tick();
    } else if (currentMode == MODE_MONITOR && WiFi.status() == WL_CONNECTED) {
        if (millis() - lastMonSample > 500) {
            lastMonSample = millis();
            signalHistory[historyIdx] = WiFi.RSSI();
            historyIdx = (historyIdx + 1) % 40;
            drawMonitor();
        }
    } else if (currentMode == MODE_PACKETS && promiscuousOn) {
        if (millis() - lastPacketSampleMs > 250) {
            lastPacketSampleMs = millis();
            // packetCounter is only ever incremented from the promiscuous
            // callback and read/reset here, both on the same core's task
            // context - not a true hardware ISR, so a plain volatile
            // read-then-clear is enough. Disabling interrupts here would be
            // overkill and risks stalling the WiFi/BT stack's own timing.
            uint32_t count = packetCounter;
            packetCounter = 0;
            packetHistory[packetHistIdx] = (int)count;
            packetHistIdx = (packetHistIdx + 1) % 40;
            if (count > packetPeak) packetPeak = count;
            drawPacketMonitor();
        }
        if (millis() - lastChannelHopMs > 700) {
            lastChannelHopMs = millis();
            if (WiFi.status() != WL_CONNECTED) {
                // Free to hop channels looking for traffic. Once actually
                // connected the channel's locked to the AP anyway, so leave
                // it alone (see maybeHopChannel note in startPacketMonitor).
                snifferChannel = (snifferChannel % 13) + 1;
                esp_wifi_set_channel(snifferChannel, WIFI_SECOND_CHAN_NONE);
            }
        }
    } else if (currentMode == MODE_BEACON_SPAM && beaconSpamActive) {
        if (millis() - lastBeaconSpamMs > BEACON_SPAM_INTERVAL_MS) {
            lastBeaconSpamMs = millis();
            sendRandomBeacon();
        }
        if (millis() - lastBeaconSpamDrawMs > 300) {
            lastBeaconSpamDrawMs = millis();
            drawBeaconSpam();
        }
    }
}

static void handleInput(const InputResult &in) {
    // Password text entry intercepts input before anything else below.
    // while typing, BACK means "erase a character" (or "cancel the edit" once
    // the field is already empty), not "leave this screen", which is
    // what the generic BACK handling further down would otherwise do.
    if (currentMode == MODE_CONNECT_FORM && formEditing) {
        if (in.type == InputEvent::CHAR) {
            if (passwordBuf.length() < 63) passwordBuf += in.ch;
            drawConnectForm();
            return;
        }
        if (in.type == InputEvent::BACK) {
            if (passwordBuf.length() > 0) passwordBuf.remove(passwordBuf.length() - 1);
            else { formEditing = false; inputSetTextEntryMode(false); }
            drawConnectForm();
            return;
        }
        if (in.type == InputEvent::OK) {
            formEditing = false;
            inputSetTextEntryMode(false);
            audioClickOk();
            drawConnectForm();
            return;
        }
        return; // swallow anything else (raw arrow codes etc.) while typing
    }

    // Swallow all input while a connect attempt is in flight. same idea as
    // the scanPending guards below, just also blocking BACK so you can't back
    // out of the screen mid-attempt and leave WiFi.begin() running unseen.
    if (currentMode == MODE_CONNECT_FORM && connecting) return;

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
        if (currentMode == MODE_PACKETS) stopPacketMonitor();
        if (currentMode == MODE_BEACON_SPAM) stopBeaconSpam();
        currentMode = MODE_MENU;
        audioClickBack();
        if (subSubMenu) subSubMenu->draw();
        return;
    }

    if (currentMode == MODE_SPECTRUM || currentMode == MODE_MONITOR) {
        if (WiFi.status() != WL_CONNECTED) return; // "connect first" screen is up, nothing to interact with
    }

    if (currentMode == MODE_CONNECT_LIST) {
        int visibleRows = (SCREEN_H - TOPBAR_HEIGHT - 20) / 18;
        if (scanPending) return;
        if (in.type == InputEvent::NAV_UP) {
            if (sel > 0) { sel--; if (sel < scrollTop) scrollTop = sel; audioClickNav(); drawConnectList(); }
        } else if (in.type == InputEvent::NAV_DOWN) {
            if (sel < netCount - 1) {
                sel++;
                if (sel >= scrollTop + visibleRows) scrollTop = sel - visibleRows + 1;
                audioClickNav(); drawConnectList();
            }
        } else if (in.type == InputEvent::OK) {
            if (netCount == 0) return;
            audioClickOk();
            int i = sortedOrder[sel];
            connectSsid = WiFi.SSID(i);
            connectOpen = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
            if (connectOpen) {
                currentMode = MODE_CONNECT_FORM;
                startConnect("");
            } else {
                passwordBuf = "";
                showPassword = false;
                formSel = 0;
                currentMode = MODE_CONNECT_FORM;
                drawConnectForm();
            }
        }
    } else if (currentMode == MODE_CONNECT_FORM) {
        if (in.type == InputEvent::NAV_UP) {
            formSel = (formSel + 2) % 3; audioClickNav(); drawConnectForm();
        } else if (in.type == InputEvent::NAV_DOWN) {
            formSel = (formSel + 1) % 3; audioClickNav(); drawConnectForm();
        } else if (in.type == InputEvent::NAV_LEFT || in.type == InputEvent::NAV_RIGHT) {
            if (formSel == 1) { showPassword = !showPassword; audioClickNav(); drawConnectForm(); }
        } else if (in.type == InputEvent::OK) {
            audioClickOk();
            if (formSel == 0) {
                formEditing = true;
                inputSetTextEntryMode(true);
                drawConnectForm();
            } else if (formSel == 1) {
                showPassword = !showPassword;
                drawConnectForm();
            } else if (formSel == 2) {
                startConnect(passwordBuf);
            }
        }
    } else if (currentMode == MODE_CONNECT_RESULT) {
        if (in.type == InputEvent::NAV_UP || in.type == InputEvent::NAV_DOWN) {
            resultSel = 1 - resultSel;
            audioClickNav();
            drawConnectResult();
        } else if (in.type == InputEvent::OK) {
            audioClickOk();
            if (resultSel == 0) {
                WiFi.disconnect();
                currentMode = MODE_MENU;
                if (subSubMenu) subSubMenu->draw();
            } else {
                currentMode = MODE_CONNECT_LIST;
                doScan();
            }
        }
    } else if (currentMode == MODE_SCANNER) {
        int visibleRows = (SCREEN_H - TOPBAR_HEIGHT - 20) / 18;
        if (scanPending) return; // ignore nav/rescan while results aren't in yet
        if (in.type == InputEvent::NAV_UP) {
            if (sel > 0) { sel--; if (sel < scrollTop) scrollTop = sel; audioClickNav(); drawScanner(); }
        } else if (in.type == InputEvent::NAV_DOWN) {
            if (sel < netCount - 1) {
                sel++;
                if (sel >= scrollTop + visibleRows) scrollTop = sel - visibleRows + 1;
                audioClickNav(); drawScanner();
            }
        } else if (in.type == InputEvent::OK) {
            audioClickOk();
            doScan(); // async now - pollScan() will redraw once results are in
        }
    } else if (currentMode == MODE_SPECTRUM) {
        if (scanPending) return;
        if (in.type == InputEvent::OK) {
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
    // In case the app is exited while an async scan or connect is still in flight.
    if (scanPending) {
        scanPending = false;
        hideLoadingOverlay();
    }
    if (connecting) {
        connecting = false;
        hideLoadingOverlay();
    }
    if (formEditing) {
        formEditing = false;
        inputSetTextEntryMode(false); // don't leak text-entry mode into whatever app opens next
    }
    stopPacketMonitor();
    stopBeaconSpam();

    WiFi.scanDelete();
    // Only kill the radio if we're not actually on a network - closing this
    // app used to always call WiFi.mode(WIFI_OFF), which silently dropped
    // any connection the moment you backed out, making "Disconnect" the only
    // way to leave the app while staying connected impossible. Leave a live
    // connection alone; it's now something you disconnect on purpose.
    if (WiFi.status() != WL_CONNECTED) {
        WiFi.mode(WIFI_OFF);
    }
}

static bool wantsExit() { return exitApp; }

}

AppModule wifiAppGet() {
    return { WifiApp::init, WifiApp::tick, WifiApp::handleInput, WifiApp::onExit, WifiApp::wantsExit };
}
