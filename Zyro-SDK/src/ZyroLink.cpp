// ============================================================================
// ZyroLink - Hardware Bridge & Teaser App for Zyro-Lite
// ============================================================================

#include "zyro_sdk_api.h"
#include "zyro_runtime.h"

static const ZyroApi *api = nullptr;
static bool exitRequested = false;

enum LinkState {
    STATE_IDLE = 0,
    STATE_BOOTING,
    STATE_CONNECTED
};

static LinkState state = STATE_IDLE;
static uint32_t sequenceStartMs = 0;
static int stepIndex = 0;
static uint32_t lastBlinkMs = 0;
static bool cursorBlink = false;

// Debug input display
static char lastKeyChar = '-';
static int lastEventType = 0;

// Terminal buffer (up to 10 lines)
static const int MAX_LOG_LINES = 10;
static char terminalLines[MAX_LOG_LINES][44];
static uint16_t terminalColors[MAX_LOG_LINES];
static int lineCount = 0;

static void clearTerminal() {
    lineCount = 0;
    for (int i = 0; i < MAX_LOG_LINES; i++) {
        terminalLines[i][0] = '\0';
        terminalColors[i] = ZYRO_RGB565(200, 200, 200);
    }
}

static void addTerminalLine(const char *msg, uint16_t color) {
    if (lineCount < MAX_LOG_LINES) {
        terminalLines[lineCount][0] = '\0';
        zyro_strcat(terminalLines[lineCount], sizeof(terminalLines[lineCount]), msg);
        terminalColors[lineCount] = color;
        lineCount++;
    } else {
        // Shift lines up
        for (int i = 0; i < MAX_LOG_LINES - 1; i++) {
            terminalLines[i][0] = '\0';
            zyro_strcat(terminalLines[i], sizeof(terminalLines[i]), terminalLines[i + 1]);
            terminalColors[i] = terminalColors[i + 1];
        }
        terminalLines[MAX_LOG_LINES - 1][0] = '\0';
        zyro_strcat(terminalLines[MAX_LOG_LINES - 1], sizeof(terminalLines[MAX_LOG_LINES - 1]), msg);
        terminalColors[MAX_LOG_LINES - 1] = color;
    }
}

static void resetToIdle() {
    state = STATE_IDLE;
    stepIndex = 0;
    sequenceStartMs = 0;
    clearTerminal();
    addTerminalLine("ZYRO HARDWARE LINK // STANDBY", ZYRO_RGB565(100, 200, 255));
    addTerminalLine("> Interface: USB-CDC [READY]", ZYRO_RGB565(140, 150, 170));
    addTerminalLine("> Press SPACE or ANY KEY to start", ZYRO_RGB565(255, 200, 80));
    if (api) {
        api->system.log("[ZYRO-LINK] Standby ready");
    }
}

static void startBootSequence() {
    state = STATE_BOOTING;
    sequenceStartMs = api->system.millis();
    stepIndex = 1;
    clearTerminal();
    addTerminalLine("== Zyro Booting ==", ZYRO_RGB565(120, 220, 255));
    if (api) {
        api->system.log("== Zyro Booting ==");
    }
}

static void appInit() {
    exitRequested = false;
    lastKeyChar = '-';
    lastEventType = 0;
    resetToIdle();
}

static void appTick() {
    uint32_t now = api->system.millis();

    // Cursor blink timer (450ms)
    if (now - lastBlinkMs > 450) {
        lastBlinkMs = now;
        cursorBlink = !cursorBlink;
    }

    // Timed progression during BOOTING
    if (state == STATE_BOOTING) {
        uint32_t elapsed = now - sequenceStartMs;

        // Step 1: Pins configured (~0.7s)
        if (stepIndex == 1 && elapsed >= 700) {
            addTerminalLine("Pins configured", ZYRO_RGB565(180, 220, 180));
            api->system.log("Pins configured");
            stepIndex = 2;
        }
        // Step 2: Modules loaded (~1.5s)
        else if (stepIndex == 2 && elapsed >= 1500) {
            addTerminalLine("Modules loaded", ZYRO_RGB565(180, 220, 180));
            api->system.log("Modules loaded");
            stepIndex = 3;
        }
        // Step 3: Status: Healthy (~2.3s)
        else if (stepIndex == 3 && elapsed >= 2300) {
            addTerminalLine("Status: Healthy", ZYRO_RGB565(80, 240, 140));
            api->system.log("Status: Healthy");
            stepIndex = 4;
        }
        // Step 4: ... (~3.0s)
        else if (stepIndex == 4 && elapsed >= 3000) {
            addTerminalLine("...", ZYRO_RGB565(140, 150, 170));
            api->system.log("...");
            stepIndex = 5;
        }
        // Step 5: Finished boot, paused 1.4s (3.0s + 1.4s = 4.4s) -> Connected!
        else if (stepIndex == 5 && elapsed >= 4400) {
            addTerminalLine("", ZYRO_RGB565(255, 255, 255));
            addTerminalLine("-- Connected to Zyro-Lite --", ZYRO_RGB565(52, 211, 153));
            addTerminalLine("Connected over USB", ZYRO_RGB565(200, 255, 220));
            addTerminalLine("Host: TDeck", ZYRO_RGB565(200, 255, 220));
            addTerminalLine("... ... ...", ZYRO_RGB565(100, 220, 180));

            api->system.log("\n-- Connected to Zyro-Lite --");
            api->system.log("Connected over USB");
            api->system.log("Host: TDeck");
            api->system.log("... ... ...\n");

            state = STATE_CONNECTED;
            stepIndex = 6;
        }
    }

    // ========================================================================
    // RENDER UI
    // ========================================================================
    // 1. Background
    api->canvas.fillRect(0, 0, api->canvas.width, api->canvas.height, ZYRO_RGB565(10, 12, 18));

    // 2. Top Header Bar
    api->canvas.fillRect(0, 0, api->canvas.width, 24, ZYRO_RGB565(18, 22, 32));
    api->canvas.drawLine(0, 24, api->canvas.width, 24, ZYRO_RGB565(60, 80, 120));

    api->canvas.setCursor(8, 7);
    api->canvas.setTextColor(ZYRO_RGB565(255, 255, 255));
    api->canvas.print("ZYRO LINK");

    api->canvas.setCursor(72, 7);
    api->canvas.setTextColor(ZYRO_RGB565(100, 160, 240));
    api->canvas.print("// HARDWARE BRIDGE");

    // Top Right Status Badge
    if (state == STATE_IDLE) {
        api->canvas.fillRect(api->canvas.width - 82, 4, 76, 16, ZYRO_RGB565(30, 40, 55));
        api->canvas.drawRect(api->canvas.width - 82, 4, 76, 16, ZYRO_RGB565(70, 100, 140));
        api->canvas.setCursor(api->canvas.width - 74, 8);
        api->canvas.setTextColor(ZYRO_RGB565(140, 200, 255));
        api->canvas.print("STANDBY");
    } else if (state == STATE_BOOTING) {
        api->canvas.fillRect(api->canvas.width - 82, 4, 76, 16, ZYRO_RGB565(60, 50, 15));
        api->canvas.drawRect(api->canvas.width - 82, 4, 76, 16, ZYRO_RGB565(220, 180, 40));
        api->canvas.setCursor(api->canvas.width - 74, 8);
        api->canvas.setTextColor(ZYRO_RGB565(255, 220, 80));
        api->canvas.print("SYNCING");
    } else {
        api->canvas.fillRect(api->canvas.width - 82, 4, 76, 16, ZYRO_RGB565(10, 50, 30));
        api->canvas.drawRect(api->canvas.width - 82, 4, 76, 16, ZYRO_RGB565(40, 200, 120));
        api->canvas.setCursor(api->canvas.width - 74, 8);
        api->canvas.setTextColor(ZYRO_RGB565(80, 255, 160));
        api->canvas.print("ACTIVE");
    }

    // 3. Hardware Info Bar
    api->canvas.fillRect(8, 30, api->canvas.width - 16, 16, ZYRO_RGB565(16, 20, 28));
    api->canvas.drawRect(8, 30, api->canvas.width - 16, 16, ZYRO_RGB565(40, 50, 70));

    api->canvas.setCursor(14, 34);
    api->canvas.setTextColor(ZYRO_RGB565(140, 150, 170));
    api->canvas.print("BUS: USB-OTG [CDC]  |  PEER: ZYRO-PRIME");

    // 4. Embedded Terminal Box
    int termX = 8;
    int termY = 50;
    int termW = api->canvas.width - 16;
    int termH = 138;

    api->canvas.fillRect(termX, termY, termW, termH, ZYRO_RGB565(5, 7, 10));
    api->canvas.drawRect(termX, termY, termW, termH, state == STATE_CONNECTED ? ZYRO_RGB565(40, 180, 100) : ZYRO_RGB565(50, 70, 100));

    // Terminal Tab Label
    api->canvas.fillRect(termX + 8, termY - 4, 110, 8, ZYRO_RGB565(5, 7, 10));
    api->canvas.setCursor(termX + 12, termY - 3);
    api->canvas.setTextColor(ZYRO_RGB565(100, 140, 190));
    api->canvas.print("CONSOLE.LOG");

    // Draw Terminal Lines
    int textY = termY + 8;
    for (int i = 0; i < lineCount; i++) {
        api->canvas.setCursor(termX + 8, textY);
        api->canvas.setTextColor(terminalColors[i]);
        api->canvas.print(terminalLines[i]);
        textY += 12;
    }

    // Blinking cursor on last line
    if (cursorBlink && textY < termY + termH - 8) {
        api->canvas.setCursor(termX + 8, textY);
        api->canvas.setTextColor(state == STATE_CONNECTED ? ZYRO_RGB565(52, 211, 153) : ZYRO_RGB565(120, 220, 255));
        api->canvas.print("_");
    }

    // 5. Bottom Controls Bar
    api->canvas.fillRect(0, api->canvas.height - 20, api->canvas.width, 20, ZYRO_RGB565(14, 16, 24));
    api->canvas.drawLine(0, api->canvas.height - 20, api->canvas.width, api->canvas.height - 20, ZYRO_RGB565(40, 50, 70));

    api->canvas.setCursor(10, api->canvas.height - 14);
    if (state == STATE_IDLE) {
        api->canvas.setTextColor(cursorBlink ? ZYRO_RGB565(255, 220, 80) : ZYRO_RGB565(180, 150, 50));
        api->canvas.print("[SPACE] Start Listen");
    } else {
        api->canvas.setTextColor(ZYRO_RGB565(140, 150, 170));
        api->canvas.print("[G] Restart Sequence");
    }

    api->canvas.setCursor(api->canvas.width - 64, api->canvas.height - 14);
    api->canvas.setTextColor(ZYRO_RGB565(160, 160, 160));
    api->canvas.print("[ESC] Exit");
}

static void appHandleInput(const ZyroInput &in) {
    lastEventType = (int)in.type;
    lastKeyChar = in.ch ? in.ch : '?';

    // ESC or Trackball Back exits cleanly
    if (in.type == ZI_BACK || in.ch == 0x1B || in.ch == 'q' || in.ch == 'Q') {
        exitRequested = true;
        return;
    }

    // Reset on G / g
    if (in.ch == 'g' || in.ch == 'G') {
        resetToIdle();
        return;
    }

    // When IDLE: ANY key, Space, Enter, or Trackball click immediately starts the boot sequence!
    if (state == STATE_IDLE) {
        if (in.type != ZI_NONE) {
            startBootSequence();
        }
    }
}

static void appOnExit() {
    if (api) {
        api->system.log("[ZYRO-LINK] App closed");
    }
}

static bool appWantsExit() {
    return exitRequested;
}

// Entrypoint
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
