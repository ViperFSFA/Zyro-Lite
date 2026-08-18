#include "app_api.h"
#include <SPI.h>
#include <SD.h>
#include <RadioLib.h>
#include "pins.h"
#include "display.h"
#include "settings.h"
#include "config.h"
#include "audio.h"
#include "menu.h"

extern SPIClass *gSharedSPI;

// Capture/replay notes: the onboard radio is an SX1262 (LoRa-oriented chip),
// not a dedicated sub-GHz OOK/ASK chip like a CC1101. That means "scan &
// capture" here works at the packet level (receive raw bytes on a frequency,
// store them) rather than a bit-for-bit raw RF replay of an arbitrary
// remote/fob's waveform.

namespace RfApp {

enum Mode {
    MODE_MENU = 0,
    MODE_SWEEP,
    MODE_SCOPE,
    MODE_CAPTURE,
    MODE_CAPTURE_ACTION,
    MODE_CAPTURE_INFO,
    MODE_CAPTURE_SAVEAS,
    MODE_CAPTURE_CONFIG,
    MODE_SAVED,
    MODE_FSK_RX,
    MODE_FSK_TX,
    MODE_FSK_MONITOR,
    MODE_FSK_BER,
    MODE_INFO_STATUS,
    MODE_INFO_CHIP,
    MODE_INFO_REGDUMP,
    MODE_INFO_SWEEP
};

enum MenuLevel {
    LEVEL_TOP = 0,
    LEVEL_FSK,
    LEVEL_INFO
};

static Mode currentMode = MODE_MENU;
static MenuLevel menuLevel = LEVEL_TOP;
static Menu *topMenu = nullptr;
static Menu *fskMenu = nullptr;
static Menu *infoMenu = nullptr;
static bool exitApp = false;

static Module *radioModule = nullptr;

class SX1262Debug : public SX1262 {
public:
    explicit SX1262Debug(Module *mod) : SX1262(mod) {}
    uint8_t dumpReg(uint16_t addr) {
        uint8_t val = 0;
        readRegister(addr, &val, 1);
        return val;
    }
    uint8_t chipStatus() { return getStatus(); }
    uint16_t chipErrors() { return getDeviceErrors(); }
};

static SX1262Debug *radio = nullptr;
static bool radioOk = false;

struct Band {
    const char *label;
    float freqMHz;
    float rssi;
};

static Band bands[4] = {
    { "315.00 MHz", 315.0f, -120.0f },
    { "433.92 MHz", 433.92f,-120.0f },
    { "868.00 MHz", 868.0f, -120.0f },
    { "915.00 MHz", 915.0f, -120.0f },
};

// Scope data
static float scopeSamples[50] = {0};
static int scopeIdx = 0;
static uint32_t lastSweepMs = 0;

static uint16_t rssiToColor565(int8_t rssi) {
    int v = constrain((int)rssi, -120, -30);
    int pct = map(v, -120, -30, 0, 255);
    uint8_t r, g, b;
    if (pct < 128) {
        r = 0;
        g = (uint8_t)map(pct, 0, 127, 0, 255);
        b = (uint8_t)map(pct, 0, 127, 255, 0);
    } else {
        r = (uint8_t)map(pct, 128, 255, 0, 255);
        g = (uint8_t)map(pct, 128, 255, 255, 0);
        b = 0;
    }
    return RGB565(r, g, b);
}

static void radioSetup() {
    if (radioOk) return;
    radioModule = new Module(RADIO_CS_PIN, RADIO_DIO1_PIN, RADIO_RST_PIN, RADIO_BUSY_PIN, *gSharedSPI);
    radio = new SX1262Debug(radioModule);

    int state = radio->begin(433.92, 125.0, 7, 5, 0x12, 10, 8, 1.6, false);
    radioOk = (state == RADIOLIB_ERR_NONE);
}

static void doSweep() {
    if (!radioOk) return;
    for (int i = 0; i < 4; i++) {
        radio->setFrequency(bands[i].freqMHz);
        radio->startReceive();
        // Adequate dwell so ambient RSSI is a real reading, not noise.
        delay(25);
        bands[i].rssi = radio->getRSSI(false);
    }

    scopeSamples[scopeIdx] = bands[1].rssi;
    scopeIdx = (scopeIdx + 1) % 50;
}

static void drawSweep() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);

    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print("Sub-GHz Ambient RSSI Sweep");

    if (!radioOk) {
        gfx->setTextColor(t.bad);
        gfx->setCursor(8, TOPBAR_HEIGHT + 24);
        gfx->print("SX1262 Radio Init Failed!");
        return;
    }

    int y = TOPBAR_HEIGHT + 24;
    for (int i = 0; i < 4; i++) {
        gfx->setTextColor(t.fg);
        gfx->setCursor(8, y);
        gfx->print(bands[i].label);

        int barX = 100;
        int barW = 150;
        int barH = 12;

        gfx->drawRect(barX, y - 2, barW, barH, t.dim);
        int fillW = map(constrain((int)bands[i].rssi, -120, -30), -120, -30, 0, barW);
        if (fillW > 0) {
            gfx->fillRect(barX + 1, y - 1, fillW, barH - 2, t.accent);
        }

        gfx->setTextColor(t.dim);
        gfx->setCursor(barX + barW + 8, y);
        gfx->print(String((int)bands[i].rssi) + "dBm");

        y += 36;
    }
}

static void drawScope() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);

    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print("433.92 MHz RSSI Scope");

    int gx = 20, gy = TOPBAR_HEIGHT + 25, gw = 280, gh = 140;
    gfx->drawRect(gx, gy, gw, gh, t.dim);

    for (int i = 0; i < 49; i++) {
        int i1 = (scopeIdx + i) % 50;
        int i2 = (scopeIdx + i + 1) % 50;

        int y1 = gy + gh - map(constrain((int)scopeSamples[i1], -120, -30), -120, -30, 0, gh);
        int y2 = gy + gh - map(constrain((int)scopeSamples[i2], -120, -30), -120, -30, 0, gh);

        int x1 = gx + (i * gw / 49);
        int x2 = gx + ((i + 1) * gw / 49);

        gfx->drawLine(x1, y1, x2, y2, t.ok);
    }

    gfx->setTextColor(t.fg);
    gfx->setCursor(gx, gy + gh + 8);
    gfx->print("Current RSSI: " + String((int)scopeSamples[(scopeIdx + 49) % 50]) + " dBm");
}

// --- Capture / Save ---
#define CAPTURE_DIR "/rf_captures"

static std::vector<String> savedFiles;
static int savedSel = 0;

static String bytesToHex(const uint8_t *data, size_t len) {
    String out;
    out.reserve(len * 2);
    const char *hexd = "0123456789ABCDEF";
    for (size_t i = 0; i < len; i++) {
        out += hexd[(data[i] >> 4) & 0xF];
        out += hexd[data[i] & 0xF];
    }
    return out;
}

static void hexToBytes(const String &hex, uint8_t *out, size_t &outLen, size_t maxLen) {
    outLen = 0;
    for (size_t i = 0; i + 1 < hex.length() && outLen < maxLen; i += 2) {
        char buf[3] = { hex[i], hex[i + 1], 0 };
        out[outLen++] = (uint8_t)strtol(buf, nullptr, 16);
    }
}

struct FoundCapture {
    float freqMhz;
    int8_t rssi;
    String hexLines;
    int packetCount;
    bool saved;
};

static std::vector<FoundCapture> foundList;
static int foundSel = 0;
static int foundScrollTop = 0;

// -1 = auto-cycle all 4 bands; otherwise pinned to bands[capBandFixed]
static int capBandFixed = -1;
static int capBandIdx = 0;
static uint32_t lastCapStepMs = 0;

// CAP_STEP_MS: how long to dwell on each band per scan cycle.
// Raised from 180ms (too short. Was picking up noise before the chip settled)
// to 600ms so the SX1262 has meaningful dwell time on each frequency and only
// fires on packets that actually persist through the window, not transient hits.
static const uint32_t CAP_STEP_MS = 600;

static const int CAP_WF_ROWS = 26;
static int8_t capWaterfall[CAP_WF_ROWS][4];

static int captureActionTarget = -1;
static Menu *captureActionMenu = nullptr;

static bool capSaveAsEditing = false;
static String capSaveAsBuf;

static Menu *captureConfigMenu = nullptr;

static int capActiveBandIdx() { return (capBandFixed >= 0) ? capBandFixed : capBandIdx; }

static void captureWaterfallReset() {
    for (int r = 0; r < CAP_WF_ROWS; r++)
        for (int c = 0; c < 4; c++) capWaterfall[r][c] = -120;
}

static void captureSessionInit() {
    foundList.clear();
    foundSel = 0;
    foundScrollTop = 0;
    capBandIdx = 0;
    lastCapStepMs = 0;
    captureWaterfallReset();
    if (radioOk) {
        radio->setFrequency(bands[capActiveBandIdx()].freqMHz);
        radio->startReceive();
    }
}

static void captureStepBand() {
    if (!radioOk) return;

    for (int r = CAP_WF_ROWS - 1; r > 0; r--) {
        memcpy(capWaterfall[r], capWaterfall[r - 1], sizeof(capWaterfall[r]));
    }

    int bandIdx = capActiveBandIdx();
    radio->setFrequency(bands[bandIdx].freqMHz);
    radio->startReceive();
    // Dwell long enough for a real packet to arrive. Must match CAP_STEP_MS
    // cadence so we're not just sampling the first few ms then switching away.
    // 50ms is enough for the chip to settle and capture a real preamble.
    delay(50);

    uint8_t buf[64];
    int state = radio->readData(buf, sizeof(buf));
    if (state == RADIOLIB_ERR_NONE) {
        size_t len = radio->getPacketLength();
        if (len > sizeof(buf)) len = sizeof(buf);
        float rssi = radio->getRSSI();
        // Basic sanity: only record signals clearly above the noise floor.
        // The SX1262 noise floor is typically around -120 dBm; require at
        // least -100 dBm so random ADC noise doesn't become a "capture".
        if ((int)rssi > -100) {
            FoundCapture fc;
            fc.freqMhz = bands[bandIdx].freqMHz;
            fc.rssi = (int8_t)constrain((int)rssi, -120, -30);
            fc.hexLines = bytesToHex(buf, len) + "\n";
            fc.packetCount = 1;
            fc.saved = false;
            foundList.push_back(fc);
            audioClickOk();
        }
        radio->startReceive();
    }

    float ambient = radio->getRSSI(false);
    capWaterfall[0][bandIdx] = (int8_t)constrain((int)ambient, -120, -30);

    if (capBandFixed < 0) capBandIdx = (capBandIdx + 1) % 4;
}

static void captureSaveEntry(int idx, const String &customName) {
    if (idx < 0 || idx >= (int)foundList.size()) return;
    SD.mkdir(CAPTURE_DIR);
    char path[80];
    if (customName.length() > 0) {
        snprintf(path, sizeof(path), CAPTURE_DIR "/%s.rfcap", customName.c_str());
    } else {
        snprintf(path, sizeof(path), CAPTURE_DIR "/capture_%lu.rfcap", (unsigned long)millis());
    }
    File f = SD.open(path, FILE_WRITE);
    if (f) {
        f.print("freq=" + String(foundList[idx].freqMhz, 2) + "\n");
        f.print(foundList[idx].hexLines);
        f.close();
        foundList[idx].saved = true;
    }
}

static void captureDeleteEntry(int idx) {
    if (idx < 0 || idx >= (int)foundList.size()) return;
    foundList.erase(foundList.begin() + idx);
    if (foundSel >= (int)foundList.size()) foundSel = max(0, (int)foundList.size() - 1);
    if (foundScrollTop > foundSel) foundScrollTop = foundSel;
}

static void refreshSavedList() {
    savedFiles.clear();
    savedSel = 0;
    File dir = SD.open(CAPTURE_DIR);
    if (!dir || !dir.isDirectory()) return;
    File entry = dir.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) savedFiles.push_back(String(entry.name()));
        entry = dir.openNextFile();
    }
    dir.close();
}

static const int CAP_LIST_W = (int)(SCREEN_W * 0.62f);
static const int CAP_ROW_H = 34;

static void drawCaptureWaterfall() {
    const Theme &t = gSettings.theme();
    int wfX = CAP_LIST_W, wfY = TOPBAR_HEIGHT, wfW = SCREEN_W - CAP_LIST_W, wfH = SCREEN_H - TOPBAR_HEIGHT;
    gfx->fillRect(wfX, wfY, wfW, wfH, t.bg);
    gfx->drawFastVLine(wfX, wfY, wfH, t.dim);

    int colW = (wfW - 4) / 4;
    int rowH = wfH / CAP_WF_ROWS;
    for (int r = 0; r < CAP_WF_ROWS; r++) {
        for (int c = 0; c < 4; c++) {
            uint16_t col = rssiToColor565(capWaterfall[r][c]);
            gfx->fillRect(wfX + 4 + c * colW, wfY + r * rowH, colW - 1, rowH, col);
        }
    }

    int activeCol = capActiveBandIdx();
    gfx->drawRect(wfX + 4 + activeCol * colW, wfY, colW - 1, wfH, t.accent);
}

static void drawCapture() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);

    if (!radioOk) {
        gfx->setTextColor(t.bad);
        gfx->setCursor(8, TOPBAR_HEIGHT + 24);
        gfx->print("SX1262 Radio Init Failed!");
        return;
    }

    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print("Scanning: " + String(bands[capActiveBandIdx()].freqMHz, 2) + " MHz");

    gfx->setTextColor(t.dim);
    gfx->setCursor(8, TOPBAR_HEIGHT + 18);
    gfx->print("OK=save/delete  C=band");

    int listTop = TOPBAR_HEIGHT + 34;
    int listH = SCREEN_H - listTop;
    int visibleRows = max(1, listH / CAP_ROW_H);

    if (foundList.empty()) {
        gfx->setTextColor(t.dim);
        gfx->setCursor(8, listTop + 6);
        gfx->print("Listening...");
    } else {
        if (foundSel < foundScrollTop) foundScrollTop = foundSel;
        if (foundSel >= foundScrollTop + visibleRows) foundScrollTop = foundSel - visibleRows + 1;

        for (int row = 0; row < visibleRows; row++) {
            int i = foundScrollTop + row;
            if (i >= (int)foundList.size()) break;
            int y = listTop + row * CAP_ROW_H;
            bool hi = (i == foundSel);
            const FoundCapture &fc = foundList[i];

            if (hi) gfx->drawRoundRect(4, y, CAP_LIST_W - 10, CAP_ROW_H - 4, 6, t.accent);
            gfx->setTextColor(hi ? t.accent : t.fg);
            gfx->setCursor(12, y + 8);
            gfx->print("Found: " + String(fc.freqMhz, 2) + " MHz");
            gfx->setTextColor(t.dim);
            gfx->setCursor(12, y + 20);
            gfx->print(String((int)fc.rssi) + " dBm" + (fc.saved ? "  [saved]" : ""));

            gfx->fillCircle(CAP_LIST_W - 16, y + 14, 4, rssiToColor565(fc.rssi));
        }

        int n = (int)foundList.size();
        if (n > visibleRows) {
            int barX = CAP_LIST_W - 6;
            gfx->fillRoundRect(barX, listTop, 3, listH, 1, t.dim);
            int thumbH = max(8, listH * visibleRows / n);
            int thumbY = listTop + (listH - thumbH) * foundScrollTop / (n - visibleRows);
            gfx->fillRoundRect(barX, thumbY, 3, thumbH, 1, t.accent);
        }
    }

    drawCaptureWaterfall();
}

static void drawCaptureInfo() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);
    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print("Capture Info (BACK=close)");

    if (captureActionTarget < 0 || captureActionTarget >= (int)foundList.size()) return;
    const FoundCapture &fc = foundList[captureActionTarget];

    gfx->setTextColor(t.fg);
    int y = TOPBAR_HEIGHT + 26;
    gfx->setCursor(8, y); gfx->print("Freq    : " + String(fc.freqMhz, 2) + " MHz"); y += 18;
    gfx->setCursor(8, y); gfx->print("RSSI    : " + String((int)fc.rssi) + " dBm"); y += 18;
    gfx->setCursor(8, y); gfx->print("Packets : " + String(fc.packetCount)); y += 18;
    gfx->setCursor(8, y); gfx->print(String("Saved   : ") + (fc.saved ? "Yes" : "No")); y += 18;

    gfx->setTextColor(t.dim);
    y += 6;
    gfx->setCursor(8, y); gfx->print("Payload (hex):"); y += 14;
    gfx->setTextColor(t.ok);
    String hexPreview = fc.hexLines;
    hexPreview.replace("\n", " ");
    gfx->setCursor(8, y); gfx->print(hexPreview.substring(0, 40));
}

static void drawCaptureSaveAs() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);
    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print("Save As");

    gfx->setTextColor(t.fg);
    gfx->setCursor(8, TOPBAR_HEIGHT + 30);
    gfx->print("Name: " + capSaveAsBuf + (capSaveAsEditing ? "_" : ""));

    gfx->setTextColor(t.dim);
    gfx->setCursor(8, TOPBAR_HEIGHT + 54);
    if (capSaveAsEditing) gfx->print("Type on keyboard, ENTER=save, BACK=erase");
    else gfx->print("OK=start typing");
    gfx->setCursor(8, TOPBAR_HEIGHT + 68);
    gfx->print("Saved as /rf_captures/<name>.rfcap");
}

static void drawSaved() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);
    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    // Removed "OK=replay". Replay is not supported on the T-Deck's SX1262.
    // Files can be viewed/deleted; use the Scan & Capture screen to add new ones.
    gfx->print("Saved Captures");

    if (savedFiles.empty()) {
        gfx->setTextColor(t.warn);
        gfx->setCursor(8, TOPBAR_HEIGHT + 26);
        gfx->print("No saved captures yet.");
        gfx->setTextColor(t.dim);
        gfx->setCursor(8, TOPBAR_HEIGHT + 46);
        gfx->print("Use Scan & Capture to record signals.");
        return;
    }

    gfx->setTextColor(t.dim);
    gfx->setCursor(8, TOPBAR_HEIGHT + 18);
    gfx->print("OK=view info  BACK=return");

    for (size_t i = 0; i < savedFiles.size(); i++) {
        int y = TOPBAR_HEIGHT + 34 + i * 18;
        bool hi = ((int)i == savedSel);
        if (hi) gfx->fillRect(4, y - 2, SCREEN_W - 8, 16, t.accent);
        gfx->setTextColor(hi ? t.accentFg : t.fg);
        gfx->setCursor(8, y);
        gfx->print(savedFiles[i]);
    }
}

// ===================== FSK Mode =====================
static float fskFreqMHz = 433.92f;
static const float FSK_BIT_RATE_KBPS = 4.8f;
static const float FSK_FREQ_DEV_KHZ = 5.0f;
static const float FSK_RX_BW_KHZ = 156.2f;
static bool fskActive = false;

static void enterFskMode() {
    if (!radioOk) return;
    int state = radio->beginFSK(fskFreqMHz, FSK_BIT_RATE_KBPS, FSK_FREQ_DEV_KHZ, FSK_RX_BW_KHZ, 10, 16, 1.6, false);
    fskActive = (state == RADIOLIB_ERR_NONE);
}

static void exitFskMode() {
    if (!radioOk || !fskActive) return;
    radio->begin(433.92, 125.0, 7, 5, 0x12, 10, 8, 1.6, false);
    fskActive = false;
}

// --- FSK Receiver ---
static uint32_t fskRxCount = 0;
static float fskLastRssi = -120.0f;
static String fskLastPayloadHex = "(none)";

static void drawFskRx() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);
    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print("FSK RX @ " + String(fskFreqMHz, 2) + " MHz");

    if (!radioOk || !fskActive) {
        gfx->setTextColor(t.bad);
        gfx->setCursor(8, TOPBAR_HEIGHT + 24);
        gfx->print("FSK modem init failed!");
        return;
    }

    gfx->setTextColor(t.fg);
    gfx->setCursor(8, TOPBAR_HEIGHT + 26);
    gfx->print("Packets Rx: " + String(fskRxCount));
    gfx->setCursor(8, TOPBAR_HEIGHT + 46);
    gfx->print("Last RSSI : " + String(fskLastRssi, 1) + " dBm");
    gfx->setCursor(8, TOPBAR_HEIGHT + 70);
    gfx->setTextColor(t.dim);
    gfx->print("Last Payload (hex):");
    gfx->setCursor(8, TOPBAR_HEIGHT + 85);
    gfx->setTextColor(t.ok);
    gfx->print(fskLastPayloadHex.substring(0, 40));
}

static void pollFskRx() {
    if (!radioOk || !fskActive) return;
    uint8_t buf[64];
    int state = radio->readData(buf, sizeof(buf));
    if (state == RADIOLIB_ERR_NONE) {
        size_t plen = radio->getPacketLength();
        if (plen > sizeof(buf)) plen = sizeof(buf);
        fskRxCount++;
        fskLastRssi = radio->getRSSI();
        fskLastPayloadHex = bytesToHex(buf, plen);
        audioClickOk();
        drawFskRx();
        radio->startReceive();
    }
}

// --- FSK TX Test ---
static uint32_t fskTxCount = 0;
static String fskTxStatus = "Ready";

static void sendFskTestPacket() {
    if (!radioOk || !fskActive) return;
    fskTxCount++;
    String pkt = "FSKTEST " + String(fskTxCount);
    int st = radio->transmit(pkt.c_str());
    fskTxStatus = (st == RADIOLIB_ERR_NONE) ? ("TX OK (" + pkt + ")") : ("TX Error (" + String(st) + ")");
    radio->startReceive();
}

static void drawFskTx() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);
    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print("FSK TX Test @ " + String(fskFreqMHz, 2) + " MHz");

    if (!radioOk || !fskActive) {
        gfx->setTextColor(t.bad);
        gfx->setCursor(8, TOPBAR_HEIGHT + 24);
        gfx->print("FSK modem init failed!");
        return;
    }

    gfx->setTextColor(t.dim);
    gfx->setCursor(8, TOPBAR_HEIGHT + 20);
    gfx->print("OK = send test packet");

    gfx->setTextColor(t.fg);
    gfx->setCursor(8, TOPBAR_HEIGHT + 34);
    gfx->print("Packets Sent: " + String(fskTxCount));
    gfx->setCursor(8, TOPBAR_HEIGHT + 58);
    gfx->print("Last Status: " + fskTxStatus);
}

// --- FSK Packet Monitor ---
struct FskLogEntry {
    String hex;
    float rssi;
};
static std::vector<FskLogEntry> fskMonitorLog;
static const size_t FSK_LOG_MAX = 7;

static void drawFskMonitor() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);
    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print("FSK Packet Monitor @ " + String(fskFreqMHz, 2) + " MHz");

    if (!radioOk || !fskActive) {
        gfx->setTextColor(t.bad);
        gfx->setCursor(8, TOPBAR_HEIGHT + 24);
        gfx->print("FSK modem init failed!");
        return;
    }

    if (fskMonitorLog.empty()) {
        gfx->setTextColor(t.dim);
        gfx->setCursor(8, TOPBAR_HEIGHT + 26);
        gfx->print("Listening...");
        return;
    }

    int y = TOPBAR_HEIGHT + 22;
    for (int i = (int)fskMonitorLog.size() - 1; i >= 0; i--) {
        const FskLogEntry &e = fskMonitorLog[i];
        gfx->setTextColor(t.fg);
        gfx->setCursor(8, y);
        String line = String((int)e.rssi) + "dBm " + e.hex.substring(0, 28);
        gfx->print(line);
        y += 16;
    }
}

static void pollFskMonitor() {
    if (!radioOk || !fskActive) return;
    uint8_t buf[64];
    int state = radio->readData(buf, sizeof(buf));
    if (state == RADIOLIB_ERR_NONE) {
        size_t plen = radio->getPacketLength();
        if (plen > sizeof(buf)) plen = sizeof(buf);
        FskLogEntry e;
        e.hex = bytesToHex(buf, plen);
        e.rssi = radio->getRSSI();
        fskMonitorLog.push_back(e);
        if (fskMonitorLog.size() > FSK_LOG_MAX) fskMonitorLog.erase(fskMonitorLog.begin());
        audioClickOk();
        drawFskMonitor();
        radio->startReceive();
    }
}

// --- FSK BER Test ---
static const uint8_t BER_PATTERN_BYTE = 0x55;
static const size_t BER_PACKET_LEN = 32;
static uint32_t berPacketsTx = 0;
static uint32_t berPacketsRx = 0;
static uint32_t berBitErrors = 0;
static uint32_t berBitsTotal = 0;

static void sendBerPacket() {
    if (!radioOk || !fskActive) return;
    uint8_t pkt[BER_PACKET_LEN];
    memset(pkt, BER_PATTERN_BYTE, BER_PACKET_LEN);
    radio->transmit(pkt, BER_PACKET_LEN);
    berPacketsTx++;
    radio->startReceive();
}

static void pollBer() {
    if (!radioOk || !fskActive) return;
    uint8_t buf[BER_PACKET_LEN];
    int state = radio->readData(buf, sizeof(buf));
    if (state == RADIOLIB_ERR_NONE) {
        size_t plen = radio->getPacketLength();
        if (plen > sizeof(buf)) plen = sizeof(buf);
        berPacketsRx++;
        for (size_t i = 0; i < plen; i++) {
            uint8_t diff = buf[i] ^ BER_PATTERN_BYTE;
            while (diff) {
                berBitErrors += (diff & 1);
                diff >>= 1;
            }
        }
        berBitsTotal += plen * 8;
        radio->startReceive();
    }
}

static void drawFskBer() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);
    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print("FSK BER Test @ " + String(fskFreqMHz, 2) + " MHz");

    if (!radioOk || !fskActive) {
        gfx->setTextColor(t.bad);
        gfx->setCursor(8, TOPBAR_HEIGHT + 24);
        gfx->print("FSK modem init failed!");
        return;
    }

    gfx->setTextColor(t.dim);
    gfx->setCursor(8, TOPBAR_HEIGHT + 20);
    gfx->print("OK = send test packet");

    gfx->setTextColor(t.fg);
    gfx->setCursor(8, TOPBAR_HEIGHT + 30);
    gfx->print("Sent: " + String(berPacketsTx) + "  Rx: " + String(berPacketsRx));
    gfx->setCursor(8, TOPBAR_HEIGHT + 52);
    gfx->print("Bit errors: " + String(berBitErrors) + " / " + String(berBitsTotal));

    gfx->setCursor(8, TOPBAR_HEIGHT + 74);
    if (berBitsTotal > 0) {
        float ber = (float)berBitErrors / (float)berBitsTotal;
        gfx->setTextColor(ber > 0.01f ? t.bad : t.ok);
        gfx->print("BER: " + String(ber * 100.0f, 3) + " %");
    } else {
        gfx->setTextColor(t.dim);
        gfx->print("BER: (no packets received yet)");
    }

    gfx->setTextColor(t.dim);
    gfx->setCursor(8, TOPBAR_HEIGHT + 100);
    gfx->print("Needs a 2nd device on this screen");
}

// ===================== Info =====================

static void drawInfoStatus() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);
    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print("Radio Status");

    if (!radioOk) {
        gfx->setTextColor(t.bad);
        gfx->setCursor(8, TOPBAR_HEIGHT + 24);
        gfx->print("SX1262 Radio Init Failed!");
        return;
    }

    gfx->setTextColor(t.fg);
    int y = TOPBAR_HEIGHT + 26;
    gfx->setCursor(8, y); gfx->print(String("Modem   : ") + (fskActive ? "FSK" : "LoRa")); y += 18;
    gfx->setCursor(8, y); gfx->print(String("Freq    : ") + String(fskActive ? fskFreqMHz : bands[1].freqMHz, 2) + " MHz"); y += 18;
    gfx->setCursor(8, y); gfx->print("Inst RSSI: " + String(radio->getRSSI(false), 1) + " dBm"); y += 18;

    char hexbuf[8];
    snprintf(hexbuf, sizeof(hexbuf), "0x%02X", radio->chipStatus());
    gfx->setCursor(8, y); gfx->print(String("Status  : ") + hexbuf); y += 18;

    snprintf(hexbuf, sizeof(hexbuf), "0x%04X", radio->chipErrors());
    gfx->setCursor(8, y); gfx->print(String("Errors  : ") + hexbuf); y += 18;
}

static void drawInfoChip() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);
    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print("SX1262 Info");

    gfx->setTextColor(t.fg);
    int y = TOPBAR_HEIGHT + 26;
    gfx->setCursor(8, y); gfx->print("Chip     : SX1262"); y += 18;
    gfx->setCursor(8, y); gfx->print("RF FW    : " RF_HW_VERSION); y += 18;
    gfx->setCursor(8, y); gfx->print("Freq rng : 150-960 MHz"); y += 18;
    gfx->setCursor(8, y); gfx->print("Last rate: " + String((int)radio->getDataRate()) + " bps"); y += 18;

    gfx->setTextColor(t.dim);
    y += 4;
    gfx->setCursor(8, y); gfx->print("Note: SX1262 reports version as"); y += 14;
    gfx->setCursor(8, y); gfx->print("\"SX1261\" over SPI (Semtech quirk)."); y += 14;
}

struct RegEntry { const char *label; uint16_t addr; };
static const RegEntry REG_LIST[] = {
    { "SyncWord0 ", RADIOLIB_SX126X_REG_SYNC_WORD_0 },
    { "SyncWord1 ", RADIOLIB_SX126X_REG_SYNC_WORD_1 },
    { "WhitenMSB ", RADIOLIB_SX126X_REG_WHITENING_INITIAL_MSB },
    { "WhitenLSB ", RADIOLIB_SX126X_REG_WHITENING_INITIAL_LSB },
    { "CrcInitMSB", RADIOLIB_SX126X_REG_CRC_INITIAL_MSB },
    { "CrcInitLSB", RADIOLIB_SX126X_REG_CRC_INITIAL_LSB },
    { "RfFreq0   ", RADIOLIB_SX126X_REG_RF_FREQUENCY_0 },
    { "RfFreq1   ", RADIOLIB_SX126X_REG_RF_FREQUENCY_1 },
    { "RfFreq2   ", RADIOLIB_SX126X_REG_RF_FREQUENCY_2 },
    { "RfFreq3   ", RADIOLIB_SX126X_REG_RF_FREQUENCY_3 },
    { "RandNum0  ", RADIOLIB_SX126X_REG_RANDOM_NUMBER_0 },
};
static const int REG_LIST_COUNT = sizeof(REG_LIST) / sizeof(REG_LIST[0]);

static void drawInfoRegdump() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);
    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print("SX1262 Register Dump");

    if (!radioOk) {
        gfx->setTextColor(t.bad);
        gfx->setCursor(8, TOPBAR_HEIGHT + 24);
        gfx->print("SX1262 Radio Init Failed!");
        return;
    }

    gfx->setTextColor(t.fg);
    int y = TOPBAR_HEIGHT + 22;
    for (int i = 0; i < REG_LIST_COUNT; i++) {
        uint8_t val = radio->dumpReg(REG_LIST[i].addr);
        char line[40];
        snprintf(line, sizeof(line), "%s 0x%04X = 0x%02X", REG_LIST[i].label, REG_LIST[i].addr, val);
        gfx->setCursor(8, y);
        gfx->print(line);
        y += 16;
    }
}

// --- Frequency sweep + waterfall (433.000-434.790 MHz) ---
static const float SWEEP_FREQ_START = 433.000f;
static const float SWEEP_FREQ_END = 434.790f;
static const int SWEEP_STEPS = 64;
static const int WATERFALL_ROWS = 22;
#define SWEEP_DIR "/rf_scans"

static float sweepFreqs[SWEEP_STEPS];
static int8_t waterfall[WATERFALL_ROWS][SWEEP_STEPS];
static float latestSweepRssi[SWEEP_STEPS];
static uint32_t lastInfoSweepMs = 0;
static bool infoSweepInited = false;
static String sweepSaveMsg;
static uint32_t sweepSaveMsgUntil = 0;

static void sweepInit() {
    if (!infoSweepInited) {
        for (int i = 0; i < SWEEP_STEPS; i++) {
            sweepFreqs[i] = SWEEP_FREQ_START + (SWEEP_FREQ_END - SWEEP_FREQ_START) * i / (SWEEP_STEPS - 1);
            latestSweepRssi[i] = -120.0f;
        }
        for (int r = 0; r < WATERFALL_ROWS; r++)
            for (int c = 0; c < SWEEP_STEPS; c++) waterfall[r][c] = -120;
        infoSweepInited = true;
    }
    lastInfoSweepMs = 0;
}

static void doInfoSweepStep() {
    if (!radioOk) return;
    for (int r = WATERFALL_ROWS - 1; r > 0; r--) {
        memcpy(waterfall[r], waterfall[r - 1], sizeof(waterfall[r]));
    }

    for (int i = 0; i < SWEEP_STEPS; i++) {
        radio->setFrequency(sweepFreqs[i]);
        radio->startReceive();
        delayMicroseconds(600);
        float r = radio->getRSSI(false);
        latestSweepRssi[i] = r;
        waterfall[0][i] = (int8_t)constrain((int)r, -120, -30);
    }
}

static void saveInfoSweepScan() {
    SD.mkdir(SWEEP_DIR);
    char path[64];
    snprintf(path, sizeof(path), SWEEP_DIR "/scan_%lu.csv", (unsigned long)millis());
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        sweepSaveMsg = "Save failed (no SD?)";
        sweepSaveMsgUntil = millis() + 1500;
        return;
    }
    f.println("freq_mhz,rssi_dbm");
    for (int i = 0; i < SWEEP_STEPS; i++) {
        f.print(String(sweepFreqs[i], 3));
        f.print(",");
        f.println(String((int)latestSweepRssi[i]));
    }
    f.close();
    sweepSaveMsg = "Saved scan to SD";
    sweepSaveMsgUntil = millis() + 1500;
}

static void drawInfoSweep() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);
    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(4, TOPBAR_HEIGHT + 2);
    if (sweepSaveMsgUntil > millis()) {
        gfx->setTextColor(t.ok);
        gfx->print(sweepSaveMsg);
    } else {
        gfx->print(String(SWEEP_FREQ_START, 3) + "-" + String(SWEEP_FREQ_END, 3) + "MHz waterfall (OK=save)");
    }

    if (!radioOk) {
        gfx->setTextColor(t.bad);
        gfx->setCursor(8, TOPBAR_HEIGHT + 24);
        gfx->print("SX1262 Radio Init Failed!");
        return;
    }

    int wfX = 0, wfY = TOPBAR_HEIGHT + 14, wfW = SCREEN_W, wfH = 154;
    int colW = wfW / SWEEP_STEPS;
    int rowH = wfH / WATERFALL_ROWS;

    for (int r = 0; r < WATERFALL_ROWS; r++) {
        for (int c = 0; c < SWEEP_STEPS; c++) {
            uint16_t col = rssiToColor565(waterfall[r][c]);
            gfx->fillRect(wfX + c * colW, wfY + r * rowH, colW, rowH, col);
        }
    }

    int legY = wfY + rowH * WATERFALL_ROWS + 6;
    gfx->setTextColor(t.dim);
    gfx->setCursor(4, legY);
    gfx->print("Weak");
    for (int i = 0; i < 40; i++) {
        gfx->drawFastVLine(46 + i * 5, legY, 10, rssiToColor565((int8_t)map(i, 0, 39, -120, -30)));
    }
    gfx->setCursor(252, legY);
    gfx->print("Strong");

    int peakIdx = 0;
    for (int i = 1; i < SWEEP_STEPS; i++) {
        if (latestSweepRssi[i] > latestSweepRssi[peakIdx]) peakIdx = i;
    }
    gfx->setTextColor(t.fg);
    gfx->setCursor(4, legY + 16);
    gfx->print("Peak: " + String(sweepFreqs[peakIdx], 3) + " MHz  " + String((int)latestSweepRssi[peakIdx]) + " dBm");
}

// --- Scan & Capture: action menu ---
static void closeCaptureActionMenu() {
    if (captureActionMenu) { delete captureActionMenu; captureActionMenu = nullptr; }
}

static void openCaptureActionMenu(int idx) {
    captureActionTarget = idx;
    closeCaptureActionMenu();
    std::vector<MenuItem> items = {
        { "Save",    ">", [](){ captureSaveEntry(captureActionTarget, ""); currentMode = MODE_CAPTURE; drawCapture(); } },
        { "Delete",  ">", [](){ captureDeleteEntry(captureActionTarget); currentMode = MODE_CAPTURE; drawCapture(); } },
        { "Save As", ">", [](){ capSaveAsBuf = ""; capSaveAsEditing = false; currentMode = MODE_CAPTURE_SAVEAS; drawCaptureSaveAs(); } },
        { "Info",    ">", [](){ currentMode = MODE_CAPTURE_INFO; drawCaptureInfo(); } },
    };
    captureActionMenu = new Menu("Capture", items);
    currentMode = MODE_CAPTURE_ACTION;
    captureActionMenu->draw();
}

static void closeCaptureConfigMenu() {
    if (captureConfigMenu) { delete captureConfigMenu; captureConfigMenu = nullptr; }
}

static void openCaptureConfigMenu() {
    closeCaptureConfigMenu();
    std::vector<MenuItem> items = {
        { "Auto (cycle all bands)", ">", [](){ capBandFixed = -1; currentMode = MODE_CAPTURE; drawCapture(); } },
        { bands[0].label,           ">", [](){ capBandFixed = 0;  currentMode = MODE_CAPTURE; drawCapture(); } },
        { bands[1].label,           ">", [](){ capBandFixed = 1;  currentMode = MODE_CAPTURE; drawCapture(); } },
        { bands[2].label,           ">", [](){ capBandFixed = 2;  currentMode = MODE_CAPTURE; drawCapture(); } },
        { bands[3].label,           ">", [](){ capBandFixed = 3;  currentMode = MODE_CAPTURE; drawCapture(); } },
    };
    captureConfigMenu = new Menu("Configure Scan", items);
    currentMode = MODE_CAPTURE_CONFIG;
    captureConfigMenu->draw();
}

// ===================== App lifecycle =====================

static void init() {
    exitApp = false;
    currentMode = MODE_MENU;
    menuLevel = LEVEL_TOP;
    radioSetup();

    if (topMenu) delete topMenu;
    std::vector<MenuItem> items = {
        { "Sub-GHz Band Sweep",  ">", [](){ currentMode = MODE_SWEEP; doSweep(); drawSweep(); } },
        { "433MHz Signal Scope", ">", [](){ currentMode = MODE_SCOPE; doSweep(); drawScope(); } },
        { "Scan & Capture",      ">", [](){ currentMode = MODE_CAPTURE; captureSessionInit(); drawCapture(); } },
        { "Saved Captures",      ">", [](){ currentMode = MODE_SAVED; refreshSavedList(); drawSaved(); } },
        { "FSK Mode",            ">", [](){ menuLevel = LEVEL_FSK; if (fskMenu) { fskMenu->forceRedraw(); fskMenu->draw(); } } },
        { "Info",                ">", [](){ menuLevel = LEVEL_INFO; if (infoMenu) { infoMenu->forceRedraw(); infoMenu->draw(); } } },
    };
    topMenu = new Menu("Sub-GHz Tools", items);

    if (fskMenu) delete fskMenu;
    std::vector<MenuItem> fskItems = {
        { "FSK Receiver",   ">", [](){
              enterFskMode(); currentMode = MODE_FSK_RX; fskRxCount = 0;
              if (fskActive) radio->startReceive();
              drawFskRx();
          } },
        { "FSK TX Test",    ">", [](){
              enterFskMode(); currentMode = MODE_FSK_TX;
              drawFskTx();
          } },
        { "Packet Monitor", ">", [](){
              enterFskMode(); currentMode = MODE_FSK_MONITOR; fskMonitorLog.clear();
              if (fskActive) radio->startReceive();
              drawFskMonitor();
          } },
        { "BER Test",       ">", [](){
              enterFskMode(); currentMode = MODE_FSK_BER;
              berPacketsTx = berPacketsRx = berBitErrors = berBitsTotal = 0;
              if (fskActive) radio->startReceive();
              drawFskBer();
          } },
    };
    fskMenu = new Menu("FSK Mode", fskItems);

    if (infoMenu) delete infoMenu;
    std::vector<MenuItem> infoItems = {
        { "Radio Status",    ">", [](){ currentMode = MODE_INFO_STATUS; drawInfoStatus(); } },
        { "SX1262 Info",     ">", [](){ currentMode = MODE_INFO_CHIP; drawInfoChip(); } },
        { "Register Dump",   ">", [](){ currentMode = MODE_INFO_REGDUMP; drawInfoRegdump(); } },
        { "Frequency Sweep", ">", [](){ currentMode = MODE_INFO_SWEEP; sweepInit(); drawInfoSweep(); } },
    };
    infoMenu = new Menu("Radio Info", infoItems);

    topMenu->draw();
}

static Menu *activeMenu() {
    if (menuLevel == LEVEL_FSK) return fskMenu;
    if (menuLevel == LEVEL_INFO) return infoMenu;
    return topMenu;
}

static void tick() {
    if (currentMode == MODE_MENU) {
        Menu *m = activeMenu();
        if (m) m->tick();
        return;
    }

    if (currentMode == MODE_CAPTURE) {
        if (millis() - lastCapStepMs > CAP_STEP_MS) {
            lastCapStepMs = millis();
            captureStepBand();
            drawCapture();
        }
        return;
    }

    if (currentMode == MODE_CAPTURE_ACTION) { if (captureActionMenu) captureActionMenu->tick(); return; }
    if (currentMode == MODE_CAPTURE_CONFIG) { if (captureConfigMenu) captureConfigMenu->tick(); return; }

    if (currentMode == MODE_FSK_RX) { pollFskRx(); return; }
    if (currentMode == MODE_FSK_MONITOR) { pollFskMonitor(); return; }
    if (currentMode == MODE_FSK_BER) { pollBer(); return; }

    if (currentMode == MODE_INFO_SWEEP) {
        if (millis() - lastInfoSweepMs > 400) {
            lastInfoSweepMs = millis();
            doInfoSweepStep();
            drawInfoSweep();
        }
        return;
    }

    if (millis() - lastSweepMs > 600) {
        lastSweepMs = millis();
        doSweep();
        if (currentMode == MODE_SWEEP) drawSweep();
        else if (currentMode == MODE_SCOPE) drawScope();
    }
}

static void handleInput(const InputResult &in) {
    if (currentMode == MODE_CAPTURE_SAVEAS && capSaveAsEditing) {
        if (in.type == InputEvent::CHAR) {
            if (capSaveAsBuf.length() < 40) capSaveAsBuf += in.ch;
            drawCaptureSaveAs();
            return;
        }
        if (in.type == InputEvent::BACK) {
            if (capSaveAsBuf.length() > 0) capSaveAsBuf.remove(capSaveAsBuf.length() - 1);
            else { capSaveAsEditing = false; inputSetTextEntryMode(false); }
            drawCaptureSaveAs();
            return;
        }
        if (in.type == InputEvent::OK) {
            capSaveAsEditing = false;
            inputSetTextEntryMode(false);
            audioClickOk();
            String name = capSaveAsBuf;
            name.trim();
            if (name.length() == 0) name = "capture_" + String((unsigned long)millis());
            captureSaveEntry(captureActionTarget, name);
            currentMode = MODE_CAPTURE;
            drawCapture();
            return;
        }
        return;
    }

    if (currentMode == MODE_MENU) {
        if (in.type == InputEvent::BACK) {
            if (menuLevel == LEVEL_TOP) {
                exitApp = true;
                audioClickBack();
                return;
            }
            if (menuLevel == LEVEL_FSK && fskActive) exitFskMode();
            menuLevel = LEVEL_TOP;
            audioClickBack();
            if (topMenu) { topMenu->forceRedraw(); topMenu->draw(); }
            return;
        }
        Menu *m = activeMenu();
        if (m) m->handleInput(in);
        return;
    }

    if (in.type == InputEvent::BACK) {
        if (currentMode == MODE_CAPTURE_ACTION) {
            closeCaptureActionMenu();
            currentMode = MODE_CAPTURE;
            audioClickBack();
            drawCapture();
            return;
        }

        if (currentMode == MODE_CAPTURE_INFO) {
            currentMode = MODE_CAPTURE_ACTION;
            audioClickBack();
            if (captureActionMenu) { captureActionMenu->forceRedraw(); captureActionMenu->draw(); }
            return;
        }

        if (currentMode == MODE_CAPTURE_SAVEAS) {
            currentMode = MODE_CAPTURE_ACTION;
            audioClickBack();
            if (captureActionMenu) { captureActionMenu->forceRedraw(); captureActionMenu->draw(); }
            return;
        }

        if (currentMode == MODE_CAPTURE_CONFIG) {
            closeCaptureConfigMenu();
            currentMode = MODE_CAPTURE;
            audioClickBack();
            drawCapture();
            return;
        }

        if (currentMode == MODE_FSK_RX || currentMode == MODE_FSK_TX ||
            currentMode == MODE_FSK_MONITOR || currentMode == MODE_FSK_BER) {
            currentMode = MODE_MENU;
            menuLevel = LEVEL_FSK;
            audioClickBack();
            if (fskMenu) { fskMenu->forceRedraw(); fskMenu->draw(); }
            return;
        }

        if (currentMode == MODE_INFO_STATUS || currentMode == MODE_INFO_CHIP ||
            currentMode == MODE_INFO_REGDUMP || currentMode == MODE_INFO_SWEEP) {
            currentMode = MODE_MENU;
            menuLevel = LEVEL_INFO;
            audioClickBack();
            if (infoMenu) { infoMenu->forceRedraw(); infoMenu->draw(); }
            return;
        }

        currentMode = MODE_MENU;
        menuLevel = LEVEL_TOP;
        audioClickBack();
        if (topMenu) topMenu->draw();
        return;
    }

    if (currentMode == MODE_CAPTURE) {
        if (in.type == InputEvent::CHAR && (in.ch == 'c' || in.ch == 'C')) {
            audioClickOk();
            openCaptureConfigMenu();
            return;
        }
        if (foundList.empty()) return;
        if (in.type == InputEvent::NAV_UP) {
            foundSel = (foundSel - 1 + (int)foundList.size()) % (int)foundList.size();
            audioClickNav(); drawCapture();
        } else if (in.type == InputEvent::NAV_DOWN) {
            foundSel = (foundSel + 1) % (int)foundList.size();
            audioClickNav(); drawCapture();
        } else if (in.type == InputEvent::OK) {
            audioClickOk();
            openCaptureActionMenu(foundSel);
        }
        return;
    }

    if (currentMode == MODE_CAPTURE_ACTION) {
        if (captureActionMenu) captureActionMenu->handleInput(in);
        return;
    }

    if (currentMode == MODE_CAPTURE_CONFIG) {
        if (captureConfigMenu) captureConfigMenu->handleInput(in);
        return;
    }

    if (currentMode == MODE_CAPTURE_SAVEAS) {
        if (in.type == InputEvent::OK) {
            audioClickOk();
            capSaveAsEditing = true;
            inputSetTextEntryMode(true);
            drawCaptureSaveAs();
        }
        return;
    }

    if (currentMode == MODE_CAPTURE_INFO) {
        return;
    }

    if (currentMode == MODE_SAVED) {
        if (savedFiles.empty()) return;
        if (in.type == InputEvent::NAV_UP) {
            savedSel = (savedSel - 1 + (int)savedFiles.size()) % savedFiles.size();
            audioClickNav(); drawSaved();
        } else if (in.type == InputEvent::NAV_DOWN) {
            savedSel = (savedSel + 1) % (int)savedFiles.size();
            audioClickNav(); drawSaved();
        }
        // OK on saved captures: no replay on T-Deck. Just a nav action, no-op for now.
        return;
    }

    if (currentMode == MODE_FSK_TX) {
        if (in.type == InputEvent::OK) {
            audioClickOk();
            sendFskTestPacket();
            drawFskTx();
        }
        return;
    }

    if (currentMode == MODE_FSK_BER) {
        if (in.type == InputEvent::OK) {
            audioClickOk();
            sendBerPacket();
            drawFskBer();
        }
        return;
    }

    if (currentMode == MODE_INFO_SWEEP) {
        if (in.type == InputEvent::OK) {
            audioClickOk();
            saveInfoSweepScan();
            drawInfoSweep();
        }
        return;
    }

    if (in.type == InputEvent::OK) {
        audioClickOk();
        doSweep();
        if (currentMode == MODE_SWEEP) drawSweep();
        else if (currentMode == MODE_SCOPE) drawScope();
    }
}

static void onExit() {
    if (capSaveAsEditing) {
        capSaveAsEditing = false;
        inputSetTextEntryMode(false);
    }
    closeCaptureActionMenu();
    closeCaptureConfigMenu();
    if (fskActive) exitFskMode();
    if (radioOk && radio) {
        radio->standby();
    }
    if (topMenu) { delete topMenu; topMenu = nullptr; }
    if (fskMenu) { delete fskMenu; fskMenu = nullptr; }
    if (infoMenu) { delete infoMenu; infoMenu = nullptr; }
}

static bool wantsExit() { return exitApp; }

}

AppModule rfAppGet() {
    return { RfApp::init, RfApp::tick, RfApp::handleInput, RfApp::onExit, RfApp::wantsExit };
}
