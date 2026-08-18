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

static void showFallbackSplash() {
    const Theme &t = gSettings.theme();
    gfx->fillScreen(t.bg);
    gfx->setTextColor(t.accent);
    gfx->setTextSize(3);
    int16_t titleW = (int)strlen(FW_NAME) * 18;
    int16_t x = (SCREEN_W - titleW) / 2;
    gfx->setCursor(x, SCREEN_H / 2 - 20);
    gfx->print(FW_NAME);

    // Small accent underline beneath the title. a cheap way to make the plain fallback splash (no SD / no GIF) 
    gfx->fillRoundRect(x, SCREEN_H / 2 + 2, titleW, 2, 1, t.accent);

    gfx->setTextSize(1);
    gfx->setTextColor(t.dim);
    gfx->setCursor(SCREEN_W / 2 - 40, SCREEN_H / 2 + 14);
    gfx->print("v" FW_VERSION);
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
                    gif.reset(); // loop the gif until the 2s window is up
                }
                // Canvas doesn't show anything until flushed - each
                // playFrame() call above just finished compositing one
                // complete GIF frame into the framebuffer, so this is the
                // right place to push it, once per frame rather than once
                // per scanline (gifDraw() runs per-scanline internally).
                gfx->flush();
            }
            gif.close();
            played = true;
        }
    }

    if (!played) {
        showFallbackSplash();
        if (!sdOk) {
            const Theme &t = gSettings.theme();
            gfx->setTextColor(t.warn);
            gfx->setCursor(20, SCREEN_H - 30);
            gfx->setTextSize(1);
            gfx->print("No SD card detected (recommended)");
        }
        gfx->flush();
        delay(SPLASH_DURATION_MS);
    }
}
