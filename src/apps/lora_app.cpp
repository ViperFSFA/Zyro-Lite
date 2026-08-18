#include "app_api.h"
#include <SPI.h>
#include <RadioLib.h>
#include <Preferences.h>
#include "pins.h"
#include "display.h"
#include "settings.h"
#include "config.h"
#include "audio.h"
#include "menu.h"

extern SPIClass *gSharedSPI;

namespace LoraApp {

enum Mode {
    MODE_MENU = 0,
    MODE_RX,
    MODE_PING,
    MODE_CHAT_SETUP,
    MODE_CHAT,
    MODE_BEACON,
    MODE_CHANNEL
};

static Mode currentMode = MODE_MENU;
static Menu *subSubMenu = nullptr;
static bool exitApp = false;

static Module *radioModule = nullptr;
static SX1262 *radio = nullptr;
static bool radioOk = false;
static volatile bool gotPacket = false;

static String lastPayload = "";
static float lastRssi = 0;
static float lastSnr = 0;
static uint32_t packetCount = 0;
static uint32_t pingCount = 0;
static String pingStatus = "Ready";
static bool autoPing = false;
static uint32_t lastAutoPingMs = 0;
static const uint32_t AUTO_PING_INTERVAL_MS = 3000;

// Shared LoRa frequency (also used/persisted by Chat setup below) - declared
// here since Channel Monitor's draw needs it and comes before the Chat
// section in this file.
static float chatFreqMHz = 868.0f;

// --- Beacon ---
// Periodic broadcast beacon: sends an identifying packet on a timer so
// other LoRa devices in range can confirm this one is alive/reachable,
// same idea as a Flipper's sub-GHz beacon mode.
static uint32_t beaconCount = 0;
static bool beaconActive = false;
static uint32_t lastBeaconMs = 0;
static const uint32_t BEACON_INTERVAL_MS = 5000;
static String beaconStatus = "Idle";

static void sendBeacon() {
    if (!radioOk) return;
    beaconCount++;
    String pkt = "BEACON|" + String(FW_NAME) + "|" + String(beaconCount);
    int st = radio->transmit(pkt.c_str());
    beaconStatus = (st == RADIOLIB_ERR_NONE) ? "TX OK" : ("TX Err " + String(st));
    radio->startReceive();
}

static void drawBeacon() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);
    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print("LoRa Beacon (OK=send now, </>=auto)");

    gfx->setTextColor(t.fg);
    gfx->setCursor(8, TOPBAR_HEIGHT + 34);
    gfx->print("Beacons Sent: " + String(beaconCount));

    gfx->setCursor(8, TOPBAR_HEIGHT + 58);
    gfx->print("Last Status: " + beaconStatus);

    gfx->setCursor(8, TOPBAR_HEIGHT + 82);
    gfx->setTextColor(beaconActive ? t.ok : t.dim);
    gfx->print(beaconActive ? ("Auto-beacon: ON (every " + String(BEACON_INTERVAL_MS / 1000) + "s)") : "Auto-beacon: OFF");
}

// --- Channel Monitor ---
// Continuous ambient RSSI monitor on the current LoRa frequency - a rolling
// scope plus a simple busy/clear call, useful as a listen-before-talk check
// before firing off a transmission of your own.
static float chanSamples[50] = {0};
static int chanIdx = 0;
static uint32_t lastChanMs = 0;
static const float CHANNEL_BUSY_THRESHOLD_DBM = -100.0f;

static void sampleChannel() {
    if (!radioOk) return;
    // Same bugfix as rf_app.cpp's Sub-GHz sweep: getRSSI() with no argument
    // reports the LAST PACKET's RSSI, not the live ambient level - useless
    // for a channel monitor, which needs to see what's happening BETWEEN
    // packets. getRSSI(false) asks for the instantaneous reading instead.
    float r = radio->getRSSI(false);
    chanSamples[chanIdx] = r;
    chanIdx = (chanIdx + 1) % 50;
}

static void drawChannelMonitor() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);
    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print(String(chatFreqMHz, 1) + " MHz Channel Monitor");

    if (!radioOk) {
        gfx->setTextColor(t.bad);
        gfx->setCursor(8, TOPBAR_HEIGHT + 24);
        gfx->print("LoRa SX1262 HW Init Failed!");
        return;
    }

    int gx = 20, gy = TOPBAR_HEIGHT + 24, gw = 280, gh = 130;
    gfx->drawRect(gx, gy, gw, gh, t.dim);
    for (int i = 0; i < 49; i++) {
        int i1 = (chanIdx + i) % 50;
        int i2 = (chanIdx + i + 1) % 50;
        int y1 = gy + gh - map(constrain((int)chanSamples[i1], -120, -30), -120, -30, 0, gh);
        int y2 = gy + gh - map(constrain((int)chanSamples[i2], -120, -30), -120, -30, 0, gh);
        int x1 = gx + (i * gw / 49);
        int x2 = gx + ((i + 1) * gw / 49);
        gfx->drawLine(x1, y1, x2, y2, t.ok);
    }

    float current = chanSamples[(chanIdx + 49) % 50];
    bool busy = current > CHANNEL_BUSY_THRESHOLD_DBM;
    gfx->setTextColor(busy ? t.bad : t.ok);
    gfx->setCursor(gx, gy + gh + 8);
    gfx->print(busy ? "Channel: BUSY" : "Channel: CLEAR");
    gfx->setTextColor(t.fg);
    gfx->setCursor(gx + 140, gy + gh + 8);
    gfx->print(String((int)current) + " dBm");
}

// Chat state. This is a simple broadcast text chat over LoRa
static String chatUsername = "";
static bool chatUsernameEditing = false;
static String chatInputBuf = "";
static int chatSetupSel = 0; // 0=username, 1=frequency, 2=start chat

struct ChatMsg {
    String user;
    String text;
    bool self; // true for messages we sent, so they can be labeled "You"
};
static std::vector<ChatMsg> chatHistory;
static const size_t CHAT_HISTORY_MAX = 60;

static void loadChatPrefs() {
    Preferences p;
    p.begin(PREFS_NAMESPACE, true);
    chatUsername = p.getString("lora_user", "");
    chatFreqMHz = p.getFloat("lora_freq", 868.0f);
    p.end();
    if (chatFreqMHz < 150.0f || chatFreqMHz > 960.0f) chatFreqMHz = 868.0f;
}

static void saveChatPrefs() {
    Preferences p;
    p.begin(PREFS_NAMESPACE, false);
    p.putString("lora_user", chatUsername);
    p.putFloat("lora_freq", chatFreqMHz);
    p.end();
}

#if defined(ESP32)
static void IRAM_ATTR onDio1() { gotPacket = true; }
#else
static void onDio1() { gotPacket = true; }
#endif

static void radioSetup() {
    if (radioOk) return;

    radioModule = new Module(RADIO_CS_PIN, RADIO_DIO1_PIN, RADIO_RST_PIN, RADIO_BUSY_PIN, *gSharedSPI);
    radio = new SX1262(radioModule);

    int state = radio->begin(868.0, 125.0, 7, 5, 0x12, 10, 8, 1.6, false);
    if (state == RADIOLIB_ERR_NONE) {
        radioOk = true;
        radio->setDio1Action(onDio1);
        radio->startReceive();
    } else {
        radioOk = false;
    }
}

static void drawRx() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);

    gfx->setTextSize(1);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    if (!radioOk) {
        gfx->setTextColor(t.bad);
        gfx->print("LoRa SX1262 HW Init Failed!");
        return;
    }

    gfx->setTextColor(t.accent);
    gfx->print("LoRa 868MHz RX Monitor");

    gfx->setTextColor(t.fg);
    gfx->setCursor(8, TOPBAR_HEIGHT + 24);
    gfx->print("Status: Listening...");

    gfx->setCursor(8, TOPBAR_HEIGHT + 44);
    gfx->print("Packets Rx: " + String(packetCount));

    gfx->setCursor(8, TOPBAR_HEIGHT + 64);
    gfx->print("Last RSSI : " + String(lastRssi, 1) + " dBm");

    gfx->setCursor(8, TOPBAR_HEIGHT + 84);
    gfx->print("Last SNR  : " + String(lastSnr, 1) + " dB");

    gfx->setCursor(8, TOPBAR_HEIGHT + 110);
    gfx->setTextColor(t.dim);
    gfx->print("Last Payload:");

    gfx->setCursor(8, TOPBAR_HEIGHT + 125);
    gfx->setTextColor(t.ok);
    gfx->print(lastPayload.length() > 0 ? lastPayload.substring(0, 36) : "(none)");
}

static void drawPing() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);

    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print("LoRa Ping Sender (SPACE=send once, </>=toggle auto)");

    gfx->setTextColor(t.fg);
    gfx->setCursor(8, TOPBAR_HEIGHT + 35);
    gfx->print("Pings Sent: " + String(pingCount));

    gfx->setCursor(8, TOPBAR_HEIGHT + 60);
    gfx->print("Last Status: " + pingStatus);

    gfx->setCursor(8, TOPBAR_HEIGHT + 85);
    gfx->setTextColor(autoPing ? t.ok : t.dim);
    gfx->print(autoPing ? "Auto-ping: ON (every 3s)" : "Auto-ping: OFF");
}

// Chat packets look like "CHAT|<username>|<message text>". Anything else
// received (pings, raw test payloads) is left alone and keeps going through
// the existing RX monitor path below.
static bool parseChatPacket(const String &raw, String &user, String &text) {
    if (!raw.startsWith("CHAT|")) return false;
    int firstBar = raw.indexOf('|');
    int secondBar = raw.indexOf('|', firstBar + 1);
    if (secondBar < 0) return false;
    user = raw.substring(firstBar + 1, secondBar);
    text = raw.substring(secondBar + 1);
    return true;
}

static void pushChatMsg(const String &user, const String &text, bool self) {
    chatHistory.push_back({ user, text, self });
    if (chatHistory.size() > CHAT_HISTORY_MAX) {
        chatHistory.erase(chatHistory.begin());
    }
}

static void drawChatSetup() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);
    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(8, TOPBAR_HEIGHT + 4);
    gfx->print("LoRa Chat Setup");

    String userValue = chatUsernameEditing
        ? (chatUsername.length() ? chatUsername : "")
        : (chatUsername.length() ? chatUsername : "(not set)");

    auto row = [&](int i, const String &label, const String &value, int y) {
        bool hi = (chatSetupSel == i);
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
    row(0, chatUsernameEditing ? "Username (typing)" : "Username", userValue, y0);
    row(1, "Frequency", String(chatFreqMHz, 1) + " MHz", y0 + 26);
    row(2, "Start Chat", "", y0 + 52);

    gfx->setTextColor(t.dim);
    gfx->setCursor(8, y0 + 80);
    if (chatUsernameEditing) gfx->print("Type on keyboard, ENTER=done, BACK=erase");
    else gfx->print("</> on Frequency to tune, OK on Start Chat to join");
}

static void drawChat() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);
    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(6, TOPBAR_HEIGHT + 2);
    gfx->print(String(chatFreqMHz, 1) + " MHz  " + chatUsername);

    // Input line pinned to the bottom, message history fills whatever's left
    // above it and always shows the newest messages (auto-scrolls itself
    // simply by always rendering the tail of chatHistory).
    int inputY = SCREEN_H - 20;
    int historyTop = TOPBAR_HEIGHT + 16;
    int lineH = 14;
    int visibleRows = (inputY - historyTop) / lineH;

    int start = (int)chatHistory.size() > visibleRows ? (int)chatHistory.size() - visibleRows : 0;
    for (int i = start; i < (int)chatHistory.size(); i++) {
        int row = i - start;
        int y = historyTop + row * lineH;
        const ChatMsg &m = chatHistory[i];
        gfx->setTextColor(m.self ? t.ok : t.fg);
        String who = m.self ? "You" : m.user;
        String line = who.substring(0, 10) + ": " + m.text;
        gfx->setCursor(6, y);
        gfx->print(line.substring(0, 52));
    }

    gfx->drawFastHLine(0, inputY - 2, SCREEN_W, t.dim);
    gfx->setTextColor(t.fg);
    gfx->setCursor(6, inputY + 4);
    String prompt = "> " + chatInputBuf;
    // Only the tail needs to be on screen once the line gets long, same idea
    // as a real chat input box scrolling text leftward as you keep typing.
    int maxChars = (SCREEN_W - 12) / 6;
    if ((int)prompt.length() > maxChars) {
        prompt = prompt.substring(prompt.length() - maxChars);
    }
    gfx->print(prompt);
}

static void sendPingPacket() {
    if (!radioOk) return;
    pingCount++;
    String pkt = "PING " + String(pingCount);
    int st = radio->transmit(pkt.c_str());
    if (st == RADIOLIB_ERR_NONE) {
        pingStatus = "TX OK (" + pkt + ")";
    } else {
        pingStatus = "TX Error (" + String(st) + ")";
    }
    radio->startReceive();
}

static void sendChatMessage() {
    if (!radioOk || chatInputBuf.length() == 0) return;
    String pkt = "CHAT|" + chatUsername + "|" + chatInputBuf;
    radio->transmit(pkt.c_str());
    radio->startReceive();
    pushChatMsg(chatUsername, chatInputBuf, true);
    chatInputBuf = "";
    drawChat();
}

// Applies the setup screen's frequency to the radio. Needs a standby/start
// cycle around it - RadioLib doesn't retune a receiver that's already
// mid-listen just because setFrequency() was called.
static void applyChatFrequency() {
    if (!radioOk) return;
    radio->standby();
    radio->setFrequency(chatFreqMHz);
    radio->startReceive();
}

static void init() {
    exitApp = false;
    currentMode = MODE_MENU;
    radioSetup();
    loadChatPrefs();

    if (subSubMenu) delete subSubMenu;
    std::vector<MenuItem> items = {
        { "LoRa Receiver",     ">", [](){ currentMode = MODE_RX; drawRx(); } },
        { "LoRa Ping Sender",  ">", [](){ currentMode = MODE_PING; drawPing(); } },
        { "LoRa Chat",         ">", [](){
              chatSetupSel = 0;
              chatUsernameEditing = false;
              currentMode = MODE_CHAT_SETUP;
              drawChatSetup();
          } },
        { "Beacon",            ">", [](){ currentMode = MODE_BEACON; drawBeacon(); } },
        { "Channel Monitor",   ">", [](){ currentMode = MODE_CHANNEL; lastChanMs = 0; drawChannelMonitor(); } }
    };
    subSubMenu = new Menu("LoRa Tools", items);
    subSubMenu->draw();
}

static void tick() {
    if (currentMode == MODE_MENU && subSubMenu) {
        subSubMenu->tick();
        return;
    }

    if (currentMode == MODE_PING && autoPing && radioOk && millis() - lastAutoPingMs >= AUTO_PING_INTERVAL_MS) {
        lastAutoPingMs = millis();
        sendPingPacket();
        drawPing();
    }

    if (currentMode == MODE_BEACON && beaconActive && radioOk && millis() - lastBeaconMs >= BEACON_INTERVAL_MS) {
        lastBeaconMs = millis();
        sendBeacon();
        drawBeacon();
    }

    if (currentMode == MODE_CHANNEL && millis() - lastChanMs > 200) {
        lastChanMs = millis();
        sampleChannel();
        drawChannelMonitor();
    }

    if (radioOk && gotPacket) {
        gotPacket = false;
        String str;
        int state = radio->readData(str);
        if (state == RADIOLIB_ERR_NONE) {
            packetCount++;
            lastPayload = str;
            lastRssi = radio->getRSSI();
            lastSnr = radio->getSNR();

            String chatUser, chatText;
            if (parseChatPacket(str, chatUser, chatText)) {
                pushChatMsg(chatUser, chatText, false);
                audioClickOk();
                if (currentMode == MODE_CHAT) drawChat();
            } else {
                audioClickOk();
                if (currentMode == MODE_RX) drawRx();
            }
        }
        radio->startReceive();
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

    // Username entry on the setup screen - same "typing swallows everything
    // except BACK/ENTER" pattern used by the Wi-Fi password field.
    if (currentMode == MODE_CHAT_SETUP && chatUsernameEditing) {
        if (in.type == InputEvent::CHAR) {
            if (chatUsername.length() < 16) chatUsername += in.ch;
            drawChatSetup();
            return;
        }
        if (in.type == InputEvent::BACK) {
            if (chatUsername.length() > 0) chatUsername.remove(chatUsername.length() - 1);
            else { chatUsernameEditing = false; inputSetTextEntryMode(false); }
            drawChatSetup();
            return;
        }
        if (in.type == InputEvent::OK) {
            chatUsernameEditing = false;
            inputSetTextEntryMode(false);
            audioClickOk();
            drawChatSetup();
            return;
        }
        return;
    }

    // Chat's input line is always "open" - typing doesn't need a separate
    // edit-mode toggle first, it just works the moment you're on this
    // screen, the way an actual chat app behaves. BACK erasing character-by-
    // character until the line's empty, then exiting the screen, is the same
    // idea as the setup screen's username field just extended to the whole
    // chat session.
    if (currentMode == MODE_CHAT) {
        if (in.type == InputEvent::CHAR) {
            if (chatInputBuf.length() < 120) chatInputBuf += in.ch;
            drawChat();
            return;
        }
        if (in.type == InputEvent::OK) {
            audioClickOk();
            sendChatMessage();
            return;
        }
        if (in.type == InputEvent::BACK) {
            if (chatInputBuf.length() > 0) {
                chatInputBuf.remove(chatInputBuf.length() - 1);
                drawChat();
            } else {
                inputSetTextEntryMode(false);
                currentMode = MODE_MENU;
                audioClickBack();
                if (subSubMenu) subSubMenu->draw();
            }
            return;
        }
        return;
    }

    if (in.type == InputEvent::BACK) {
        currentMode = MODE_MENU;
        audioClickBack();
        if (subSubMenu) subSubMenu->draw();
        return;
    }

    if (currentMode == MODE_PING) {
        if (in.type == InputEvent::OK || in.type == InputEvent::CHAR) {
            audioClickOk();
            sendPingPacket();
            drawPing();
        } else if (in.type == InputEvent::NAV_LEFT || in.type == InputEvent::NAV_RIGHT) {
            autoPing = !autoPing;
            lastAutoPingMs = millis();
            audioClickNav();
            drawPing();
        }
    } else if (currentMode == MODE_BEACON) {
        if (in.type == InputEvent::OK || in.type == InputEvent::CHAR) {
            audioClickOk();
            sendBeacon();
            drawBeacon();
        } else if (in.type == InputEvent::NAV_LEFT || in.type == InputEvent::NAV_RIGHT) {
            beaconActive = !beaconActive;
            lastBeaconMs = millis();
            audioClickNav();
            drawBeacon();
        }
    } else if (currentMode == MODE_CHANNEL) {
        // Just a passive scope - nothing to interact with besides BACK,
        // which the shared BACK handler above already routes back to the menu.
    } else if (currentMode == MODE_CHAT_SETUP) {
        if (in.type == InputEvent::NAV_UP) {
            chatSetupSel = (chatSetupSel + 2) % 3; audioClickNav(); drawChatSetup();
        } else if (in.type == InputEvent::NAV_DOWN) {
            chatSetupSel = (chatSetupSel + 1) % 3; audioClickNav(); drawChatSetup();
        } else if (in.type == InputEvent::NAV_LEFT || in.type == InputEvent::NAV_RIGHT) {
            if (chatSetupSel == 1) {
                float step = (in.type == InputEvent::NAV_RIGHT) ? 0.1f : -0.1f;
                chatFreqMHz = constrain(chatFreqMHz + step, 150.0f, 960.0f);
                audioClickNav();
                drawChatSetup();
            }
        } else if (in.type == InputEvent::OK) {
            audioClickOk();
            if (chatSetupSel == 0) {
                chatUsernameEditing = true;
                inputSetTextEntryMode(true);
                drawChatSetup();
            } else if (chatSetupSel == 2) {
                if (chatUsername.length() == 0) chatUsername = "Anon";
                saveChatPrefs();
                applyChatFrequency();
                chatInputBuf = "";
                inputSetTextEntryMode(true); // stays on for the whole chat session
                currentMode = MODE_CHAT;
                drawChat();
            }
        }
    }
}

static void onExit() {
    if (chatUsernameEditing || currentMode == MODE_CHAT) {
        inputSetTextEntryMode(false); // don't leak text-entry mode into whatever app opens next
    }
    if (radioOk && radio) {
        radio->standby();
    }
    if (subSubMenu) {
        delete subSubMenu;
        subSubMenu = nullptr;
    }
}

static bool wantsExit() { return exitApp; }

}

AppModule loraAppGet() {
    return { LoraApp::init, LoraApp::tick, LoraApp::handleInput, LoraApp::onExit, LoraApp::wantsExit };
}
