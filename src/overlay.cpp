#include "overlay.h"
#include "display.h"
#include "settings.h"
#include "config.h"
#include "globe_64_64_28f.h"
#include "warning_blink_64_64_28f.h"

// Reads bit (x,y) out of a 64x64 (ICON_SRC_SIZE) 1bpp MSB-first bitmap frame
// (same layout as menu.cpp's row icons) and plots a nearest-neighbor scaled
// copy of it into a `size` x `size` box at (x, y). Only "on" bits are
// plotted (transparent background).
static void drawScaledFrame(const uint8_t *frame, int x, int y, int size, uint16_t color) {
    const int rowBytes = (ICON_SRC_SIZE + 7) / 8;
    for (int dy = 0; dy < size; dy++) {
        int sy = dy * ICON_SRC_SIZE / size;
        for (int dx = 0; dx < size; dx++) {
            int sx = dx * ICON_SRC_SIZE / size;
            uint8_t b = pgm_read_byte(&frame[sy * rowBytes + (sx / 8)]);
            if ((b >> (7 - (sx % 8))) & 0x01) {
                gfx->drawPixel(x + dx, y + dy, color);
            }
        }
    }
}


static bool loadingActive = false;
static char loadingLabel[40] = {0};
static uint32_t lastLoadingFrameMs = 0;
static int loadingSpinnerX = 0, loadingSpinnerY = 0;

void showLoadingOverlay(const char *label) {
    loadingActive = true;
    strncpy(loadingLabel, label, sizeof(loadingLabel) - 1);
    loadingLabel[sizeof(loadingLabel) - 1] = '\0';

    const Theme &t = gSettings.theme();
    int areaY = TOPBAR_HEIGHT;
    int areaH = SCREEN_H - TOPBAR_HEIGHT;

    // One full clear + the (static) label text, drawn once here. The
    // per-frame tick only needs to touch the spinner's own small square from
    // here on, instead of re-clearing/re-drawing the whole content area on
    // every single frame. see loadingOverlayTick() for why that matters.
    gfx->fillRect(0, areaY, SCREEN_W, areaH, t.bg);

    loadingSpinnerX = (SCREEN_W - OVERLAY_SPINNER_SIZE) / 2;
    loadingSpinnerY = areaY + (areaH - OVERLAY_SPINNER_SIZE) / 2 - 10;

    gfx->setTextSize(1);
    gfx->setTextColor(t.dim);
    int textW = strlen(loadingLabel) * 6;
    gfx->setCursor(max(0, (SCREEN_W - textW) / 2), loadingSpinnerY + OVERLAY_SPINNER_SIZE + 12);
    gfx->print(loadingLabel);

    lastLoadingFrameMs = 0; // force the first spinner frame to draw immediately
    loadingOverlayTick();
}

void hideLoadingOverlay() {
    loadingActive = false;
}

bool loadingOverlayActive() { return loadingActive; }

void loadingOverlayTick() {
    if (!loadingActive) return;
    if (millis() - lastLoadingFrameMs < OVERLAY_SPINNER_FRAME_MS) return;
    lastLoadingFrameMs = millis();

    const Theme &t = gSettings.theme();
    int frame = (millis() / OVERLAY_SPINNER_FRAME_MS) % ICON_FRAME_COUNT;

    // Only clear the spinner's own square, not the whole content area. the
    // same fix that made the menu's icon/glide animation stop flickering
    // applies here: never re-clear more of the screen than actually changes.
    gfx->fillRect(loadingSpinnerX, loadingSpinnerY, OVERLAY_SPINNER_SIZE, OVERLAY_SPINNER_SIZE, t.bg);
    drawScaledFrame(globe_64_64_28f_frames[frame], loadingSpinnerX, loadingSpinnerY, OVERLAY_SPINNER_SIZE, t.accent);
}

// Alert overlay
static bool alertOn = false;
static char alertMsg[64] = {0};
static uint32_t alertStartMs = 0;
static uint32_t lastAlertFrameMs = 0;
static bool (*alertResolvedCheck)() = nullptr;
static void (*alertOnDismiss)() = nullptr;

static void alertDrawFrame(int frame, int secondsLeft) {
    const Theme &t = gSettings.theme();

    // This is a hard-stop screen, so it blanks EVERYTHING (topbar included).
    // not just the content area.
    gfx->fillScreen(t.bg);

    int iconX = (SCREEN_W - ALERT_ICON_SIZE) / 2;
    int iconY = 14;
    drawScaledFrame(warning_blink_64_64_28f_frames[frame], iconX, iconY, ALERT_ICON_SIZE, t.bad);

    int textY = iconY + ALERT_ICON_SIZE + 14;
    // UI_FONT (see display.h) for the alert message - a single short line
    // with plenty of headroom, unlike the dense per-app diagnostic screens.
    // It's proportional, so measure the real width via getTextBounds()
    // instead of guessing px-per-char, and reset to the built-in font
    // immediately after: gfx's font is shared global state, and the
    // countdown line right below still wants the compact font.
    gfx->setFont(UI_FONT);
    gfx->setTextSize(1);
    gfx->setTextColor(t.bad);
    int16_t bx, by; uint16_t bw, bh;
    gfx->getTextBounds(alertMsg, 0, 0, &bx, &by, &bw, &bh);
    gfx->setCursor(max(4, (SCREEN_W - (int)bw) / 2), uiFontTextBaselineY(textY, 22, alertMsg));
    gfx->print(alertMsg);
    gfx->setFont(NULL);

    char cd[16];
    snprintf(cd, sizeof(cd), "%ds", secondsLeft);
    gfx->setTextSize(1);
    gfx->setTextColor(t.dim);
    int cdW = strlen(cd) * 6;
    gfx->setCursor((SCREEN_W - cdW) / 2, textY + 26);
    gfx->print(cd);
}

void showAlert(const char *message, bool (*resolvedCheck)(), void (*onDismiss)()) {
    alertOn = true;
    strncpy(alertMsg, message, sizeof(alertMsg) - 1);
    alertMsg[sizeof(alertMsg) - 1] = '\0';
    alertStartMs = millis();
    lastAlertFrameMs = 0; // force the first frame to draw immediately
    alertResolvedCheck = resolvedCheck;
    alertOnDismiss = onDismiss;
    alertTick();
}

bool alertActive() { return alertOn; }

static void alertClose() {
    alertOn = false;
    if (alertOnDismiss) alertOnDismiss();
}

void alertTick() {
    if (!alertOn) return;

    uint32_t elapsed = millis() - alertStartMs;
    if (elapsed >= ALERT_DURATION_MS) {
        alertClose();
        return;
    }

    if (millis() - lastAlertFrameMs < ALERT_FRAME_MS) return;
    lastAlertFrameMs = millis();

    // Polled at the same ~80ms cadence as the animation frame rather than
    // every single loop() iteration. resolvedCheck() may do real work (e.g.
    // an SD card filesystem check), and 80ms is still effectively instant
    // from a person's point of view. This is what lets the alert clear
    // itself early the moment the problem goes away instead of always
    // sitting there for the full 5 seconds.
    if (alertResolvedCheck && alertResolvedCheck()) {
        alertClose();
        return;
    }

    int frame = (millis() / ALERT_FRAME_MS) % ICON_FRAME_COUNT;
    int secondsLeft = (ALERT_DURATION_MS - elapsed + 999) / 1000; 
    alertDrawFrame(frame, secondsLeft);
}
