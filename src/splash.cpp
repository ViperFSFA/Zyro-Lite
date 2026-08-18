#include "splash.h"
#include <AnimatedGIF.h>
#include <SD.h>
#include <jpeg_decoder.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "display.h"
#include "config.h"
#include "theme.h"
#include "settings.h"
#include "audio.h"

static AnimatedGIF gif;
static File gifFile;

static void *gifOpen(const char *name, int32_t *size) {
    gifFile = SD.open(name, FILE_READ);
    if (!gifFile) return nullptr;
    *size = gifFile.size();
    return (void *)&gifFile;
}
static void gifClose(void *h) { gifFile.close(); }
static int32_t gifRead(GIFFILE *f, uint8_t *buf, int32_t len) {
    File *fp = (File *)f->fHandle;
    if (!fp || !*fp) return 0;
    return fp->read(buf, len);
}
static int32_t gifSeek(GIFFILE *f, int32_t pos) {
    File *fp = (File *)f->fHandle;
    if (!fp || !*fp) return 0;
    fp->seek(pos);
    return pos;
}

static void gifDraw(GIFDRAW *draw) {
    uint16_t lineBuf[SCREEN_W];
    int y = draw->iY + draw->y;
    int width = draw->iWidth;
    if (width > SCREEN_W) width = SCREEN_W;

    uint8_t *s = draw->pPixels;
    uint16_t *palette = (uint16_t *)draw->pPalette;

    for (int x = 0; x < width; x++) {
        uint8_t idx = s[x];
        if (draw->ucHasTransparency && idx == draw->ucTransparent) {
            lineBuf[x] = gSettings.theme().bg; // let theme bg show through transparent px
        } else {
            lineBuf[x] = palette[idx];
        }
    }
    gfx->draw16bitRGBBitmap(draw->iX, y, lineBuf, width, 1);
}

static bool loadFileToMemory(const char *path, uint8_t **outData, size_t *outSize) {
    File file = SD.open(path, FILE_READ);
    if (!file) return false;

    size_t size = file.size();
    uint8_t *data = (uint8_t *)malloc(size);
    if (!data) {
        file.close();
        return false;
    }

    if (file.read(data, size) != size) {
        free(data);
        file.close();
        return false;
    }

    file.close();
    *outData = data;
    *outSize = size;
    return true;
}

static bool isBootImageName(const char *name) {
    static const char *const bootNames[] = {"boot.jpg", "boot.jpeg", "boot.png", "boot.bmp"};
    for (size_t i = 0; i < sizeof(bootNames) / sizeof(bootNames[0]); ++i) {
        if (strcasecmp(name, bootNames[i]) == 0) return true;
    }
    return false;
}

static bool findBootImagePath(char *outPath, size_t outSize) {
    static const char *const dirs[] = {"/", "/sd_card_files"};
    for (size_t d = 0; d < sizeof(dirs) / sizeof(dirs[0]); ++d) {
        File dir = SD.open(dirs[d]);
        if (!dir || !dir.isDirectory()) continue;

        while (File entry = dir.openNextFile()) {
            if (!entry || entry.isDirectory()) continue;
            if (!isBootImageName(entry.name())) continue;

            if (strcmp(dirs[d], "/") == 0) {
                snprintf(outPath, outSize, "/%s", entry.name());
            } else {
                snprintf(outPath, outSize, "%s/%s", dirs[d], entry.name());
            }
            return true;
        }
    }
    return false;
}

static bool showBootImageSplash() {
    char path[64];
    if (!findBootImagePath(path, sizeof(path))) {
        return false;
    }

    uint8_t *jpegData = nullptr;
    size_t jpegDataSize = 0;
    if (!loadFileToMemory(path, &jpegData, &jpegDataSize)) {
        return false;
    }

    esp_jpeg_image_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.indata = jpegData;
    cfg.indata_size = (uint32_t)jpegDataSize;

    esp_jpeg_image_output_t info;
    if (esp_jpeg_get_image_info(&cfg, &info) != ESP_OK) {
        free(jpegData);
        return false;
    }

    uint8_t *rgb888 = (uint8_t *)malloc((size_t)info.width * info.height * 3);
    if (!rgb888) {
        free(jpegData);
        return false;
    }

    cfg.outbuf = rgb888;
    cfg.outbuf_size = (uint32_t)((size_t)info.width * info.height * 3);
    cfg.out_format = JPEG_IMAGE_FORMAT_RGB888;

    if (esp_jpeg_decode(&cfg, &info) != ESP_OK) {
        free(rgb888);
        free(jpegData);
        return false;
    }

    const float scale = min((float)SCREEN_W / (float)info.width, (float)SCREEN_H / (float)info.height);
    int32_t scaledW = (int32_t)roundf(info.width * scale);
    int32_t scaledH = (int32_t)roundf(info.height * scale);
    if (scaledW < 1) scaledW = 1;
    if (scaledH < 1) scaledH = 1;
    const int16_t outW = (int16_t)scaledW;
    const int16_t outH = (int16_t)scaledH;
    uint16_t *bitmap = (uint16_t *)malloc((size_t)outW * outH * sizeof(uint16_t));
    if (!bitmap) {
        free(rgb888);
        free(jpegData);
        return false;
    }

    for (int16_t y = 0; y < outH; ++y) {
        const float srcY = (float)y / (float)outH * (float)(info.height - 1);
        for (int16_t x = 0; x < outW; ++x) {
            const float srcX = (float)x / (float)outW * (float)(info.width - 1);
            const uint32_t srcIndex = (uint32_t)srcY * info.width + (uint32_t)srcX;
            const uint8_t *pixel = rgb888 + srcIndex * 3;
            bitmap[y * outW + x] = ((uint16_t)(pixel[0] & 0xF8) << 8) |
                                    ((uint16_t)(pixel[1] & 0xFC) << 3) |
                                    (pixel[2] >> 3);
        }
    }

    const Theme &t = gSettings.theme();
    gfx->fillScreen(t.bg);
    int16_t x = (SCREEN_W - outW) / 2;
    int16_t y = (SCREEN_H - outH) / 2;
    gfx->draw16bitRGBBitmap(x, y, bitmap, outW, outH);
    gfx->flush();
    delay(SPLASH_DURATION_MS);

    free(bitmap);
    free(rgb888);
    free(jpegData);
    return true;
}

#ifndef ZYRO_RGB565
#define ZYRO_RGB565(r, g, b) ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
#endif

static void showCyberpunkBootScreen(bool sdOk) {
    uint16_t cBg       = ZYRO_RGB565(6, 9, 15);      // #06090F space dark
    uint16_t cFrame    = ZYRO_RGB565(30, 45, 70);    // #1E2D46 slate blue frame
    uint16_t cAccent   = ZYRO_RGB565(52, 211, 153);  // #34D399 emerald green
    uint16_t cCyan     = ZYRO_RGB565(103, 232, 249); // #67E8F9 cyan
    uint16_t cText     = ZYRO_RGB565(232, 237, 245); // #E8EDF5 off-white
    uint16_t cMuted    = ZYRO_RGB565(138, 155, 184); // #8A9BB8
    uint16_t cWarn     = ZYRO_RGB565(251, 191, 36);  // #FBBF24 amber warning
    uint16_t cTerminal = ZYRO_RGB565(10, 14, 22);    // #0A0E16 deep console

    struct BootStep {
        const char *tag;
        const char *msg;
        uint16_t tagColor;
        uint16_t msgColor;
        uint16_t delayMs;
    };

    const BootStep steps[] = {
        { "INIT", "Initializing Zyro-Lite", cCyan, cCyan, 120 },
        { "CPU ", "Dual-Core Xtensa LX7 @ 240MHz", cAccent, cText, 100 },
        { "MEM ", "PSRAM 8MB OK", cAccent, cText, 100 },
        { "DISK", sdOk ? "SD Storage: Mounted" : "SD Storage: Standalone Mode", sdOk ? cAccent : cWarn, sdOk ? cText : cWarn, 120 },
        { "DISP", "ST7789V 320x240 RGB565", cAccent, cText, 100 },
        { "RADIO", "SX1262 Sub-GHz", cAccent, cText, 120 },
        { "INPUT", "T-Deck I2C Ready", cAccent, cText, 120 },
        { "SYS ", "All Systems Healthy", cAccent, cCyan, 160 }
    };

    int totalSteps = sizeof(steps) / sizeof(steps[0]);

    // Initial background clear
    gfx->fillRect(0, 0, SCREEN_W, SCREEN_H, cBg);

    // Double-line outer cyber frame
    gfx->drawRect(4, 4, SCREEN_W - 8, SCREEN_H - 8, cFrame);
    gfx->drawRect(5, 5, SCREEN_W - 10, SCREEN_H - 10, cFrame);

    // Glowing cyan corner brackets
    auto drawCorner = [](int x, int y, int dx, int dy) {
        gfx->fillRect(x, y, 10 * dx, 2 * dy, ZYRO_RGB565(103, 232, 249));
        gfx->fillRect(x, y, 2 * dx, 10 * dy, ZYRO_RGB565(103, 232, 249));
    };
    drawCorner(4, 4, 1, 1);
    drawCorner(SCREEN_W - 5, 4, -1, 1);
    drawCorner(4, SCREEN_H - 5, 1, -1);
    drawCorner(SCREEN_W - 5, SCREEN_H - 5, -1, -1);

    // Header bar
    gfx->fillRect(10, 10, SCREEN_W - 20, 24, cTerminal);
    gfx->drawRect(10, 10, SCREEN_W - 20, 24, cFrame);

    gfx->setTextSize(1);
    gfx->setCursor(18, 18);
    gfx->setTextColor(cCyan);
    gfx->print("ZYRO-LITE OS");

    gfx->setCursor(105, 18);
    gfx->setTextColor(cMuted);
    gfx->print("T-DECK HARDWARE BRIDGE");

    // Status Badge
    int badgeW = 76;
    int badgeH = 16;
    int badgeX = SCREEN_W - badgeW - 16;
    int badgeY = 14;
    gfx->fillRect(badgeX, badgeY, badgeW, badgeH, cBg);
    gfx->drawRect(badgeX, badgeY, badgeW, badgeH, cWarn);
    gfx->setCursor(badgeX + 8, badgeY + 4);
    gfx->setTextColor(cWarn);
    gfx->print("BOOTING");

    // Main Terminal Console Box
    int termX = 10;
    int termY = 38;
    int termW = SCREEN_W - 20;
    int termH = 154;

    gfx->fillRect(termX, termY, termW, termH, cTerminal);
    gfx->drawRect(termX, termY, termW, termH, cFrame);

    // Tab Label
    gfx->fillRect(termX + 10, termY - 4, 110, 8, cBg);
    gfx->setCursor(termX + 14, termY - 3);
    gfx->setTextColor(cMuted);
    gfx->print("SYSTEM_DIAG.LOG");

    // Bottom Progress Bar Box
    int barX = 10;
    int barY = 198;
    int barW = SCREEN_W - 20;
    int barH = 26;
    gfx->fillRect(barX, barY, barW, barH, cTerminal);
    gfx->drawRect(barX, barY, barW, barH, cFrame);

    gfx->flush();

    // Type out each diagnostic boot step
    for (int s = 0; s < totalSteps; s++) {
        const BootStep &st = steps[s];

        int lineY = termY + 8 + s * 18;

        // Tag box [ INIT ] / [ CPU ]
        gfx->fillRect(termX + 8, lineY, 44, 14, cBg);
        gfx->drawRect(termX + 8, lineY, 44, 14, st.tagColor);
        gfx->setCursor(termX + 12, lineY + 3);
        gfx->setTextColor(st.tagColor);
        gfx->print(st.tag);

        // Step description
        gfx->setCursor(termX + 58, lineY + 3);
        gfx->setTextColor(st.msgColor);
        gfx->print(st.msg);

        // Update progress bar
        int pct = ((s + 1) * 100) / totalSteps;
        int fillW = (pct * (barW - 4)) / 100;
        gfx->fillRect(barX + 2, barY + 2, barW - 4, barH - 4, cTerminal);
        if (fillW > 0) {
            gfx->fillRect(barX + 2, barY + 2, fillW, barH - 4, cAccent);
        }

        // Percentage text inside bar
        char pctBuf[28];
        snprintf(pctBuf, sizeof(pctBuf), "INITIALIZING %d%%", pct);
        int txtX = barX + (barW - strlen(pctBuf) * 6) / 2;
        gfx->setCursor(txtX, barY + 9);
        gfx->setTextColor((pct > 52) ? cTerminal : cText);
        gfx->print(pctBuf);

        audioClickNav();
        gfx->flush();
        delay(st.delayMs);
    }

    // Final READY state badge
    badgeX = SCREEN_W - badgeW - 16;
    badgeY = 14;
    gfx->fillRect(badgeX, badgeY, badgeW, badgeH, cBg);
    gfx->drawRect(badgeX, badgeY, badgeW, badgeH, cAccent);
    gfx->setCursor(badgeX + 14, badgeY + 4);
    gfx->setTextColor(cAccent);
    gfx->print("READY");
    gfx->flush();
    delay(400);
}

static void showRotatingDotsSplash() {
    uint16_t cBg     = ZYRO_RGB565(8, 11, 18);
    uint16_t cText   = ZYRO_RGB565(240, 244, 250);
    uint16_t cDim    = ZYRO_RGB565(120, 135, 160);
    uint16_t cAccent = ZYRO_RGB565(52, 211, 153);
    uint16_t cCyan   = ZYRO_RGB565(103, 232, 249);

    int cx = SCREEN_W / 2;
    int cy = SCREEN_H / 2 + 15;
    int r = 22;

    uint32_t start = millis();
    int frame = 0;

    while (millis() - start < SPLASH_DURATION_MS) {
        gfx->fillRect(0, 0, SCREEN_W, SCREEN_H, cBg);

        // Title
        gfx->setTextSize(3);
        gfx->setTextColor(cText);
        int titleW = (int)strlen(FW_NAME) * 18;
        gfx->setCursor((SCREEN_W - titleW) / 2, SCREEN_H / 2 - 50);
        gfx->print(FW_NAME);

        // Subtitle / Version
        gfx->setTextSize(1);
        gfx->setTextColor(cDim);
        gfx->setCursor((SCREEN_W - 48) / 2, SCREEN_H / 2 - 20);
        gfx->print("v" FW_VERSION);

        // Rotating dots (8 dots in a circle)
        for (int i = 0; i < 8; i++) {
            float angle = ((frame + i) % 8) * (2.0f * 3.14159f / 8.0f);
            int dx = (int)(cx + r * cos(angle));
            int dy = (int)(cy + r * sin(angle));

            int dotRadius = (i == 7) ? 4 : ((i >= 5) ? 3 : 2);
            uint16_t dotCol = (i == 7) ? cAccent : ((i >= 5) ? cCyan : cDim);

            gfx->fillCircle(dx, dy, dotRadius, dotCol);
        }

        gfx->flush();
        frame = (frame + 1) % 8;
        delay(60);
    }
}

void showSplash(bool sdOk) {
    bool played = false;

    if (showBootImageSplash()) {
        played = true;
    }

    if (!played && sdOk && SD.exists(SPLASH_GIF_PATH)) {
        gif.begin(GIF_PALETTE_RGB565_LE);
        if (gif.open(SPLASH_GIF_PATH, gifOpen, gifClose, gifRead, gifSeek, gifDraw)) {
            uint32_t start = millis();
            while (millis() - start < SPLASH_DURATION_MS) {
                if (!gif.playFrame(true, NULL)) {
                    gif.reset();
                }
                gfx->flush();
            }
            gif.close();
            played = true;
        }
    }

    if (!played) {
        if (gSettings.enableBootLog) {
            showCyberpunkBootScreen(sdOk);
        } else {
            showRotatingDotsSplash();
        }
    }
}
