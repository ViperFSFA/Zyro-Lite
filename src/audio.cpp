#include "audio.h"
#include <Arduino.h>
#include <driver/i2s.h>
#include "pins.h"
#include "settings.h"

static bool i2sReady = false;
static bool i2sInitAttempted = false;
static uint32_t lastBeepMs = 0;

// Pre-computed lightweight click buffer (16 samples click + 240 samples silence)
static int16_t clickBuf[256];
static bool clickBufInited = false;

static void initClickBuf() {
    if (clickBufInited) return;
    for (int i = 0; i < 16; i++) {
        clickBuf[i] = (i % 2 == 0) ? 3000 : -3000;
    }
    for (int i = 16; i < 256; i++) {
        clickBuf[i] = 0;
    }
    clickBufInited = true;
}

// Install the I2S driver. Can be called again after a forced uninstall
// (e.g. after BLE deinit, which on some ESP-IDF versions resets peripheral
// state). Returns true on success.
static bool i2sInstall() {
    i2s_config_t i2s_config = {};
    i2s_config.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    i2s_config.sample_rate          = 8000;
    i2s_config.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
    i2s_config.channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT;
    i2s_config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    i2s_config.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
    i2s_config.dma_buf_count        = 8;
    i2s_config.dma_buf_len          = 64;
    i2s_config.use_apll             = false;
    i2s_config.tx_desc_auto_clear   = true;

    if (i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL) != ESP_OK) {
        Serial.println("[audio] i2s_driver_install failed");
        return false;
    }

    i2s_pin_config_t pin_config = {};
    pin_config.bck_io_num   = (gpio_num_t)BOARD_I2S_BCK;
    pin_config.ws_io_num    = (gpio_num_t)BOARD_I2S_WS;
    pin_config.data_out_num = (gpio_num_t)BOARD_I2S_DOUT;
    pin_config.data_in_num  = I2S_PIN_NO_CHANGE;

    if (i2s_set_pin(I2S_NUM_0, &pin_config) != ESP_OK) {
        Serial.println("[audio] i2s_set_pin failed");
        i2s_driver_uninstall(I2S_NUM_0);
        return false;
    }

    i2s_zero_dma_buffer(I2S_NUM_0);
    Serial.println("[audio] I2S driver installed OK");
    return true;
}

void audioInit() {
    if (i2sReady) return;
    // Allow re-init after a previous failure. BLE teardown can reset I2S state
    // even mid-session, so don't gate on i2sInitAttempted here.

    pinMode(BOARD_POWERON, OUTPUT);
    digitalWrite(BOARD_POWERON, HIGH);

    i2sReady = i2sInstall();
    i2sInitAttempted = true;
}

// Ensure I2S is (still) running. If the driver was silently reset (by BLE
// deinit or any other peripheral interaction) this will reinstall it.
static void audioEnsureReady() {
    if (i2sReady) return;
    // Uninstall defensively first in case a partial init is lingering.
    i2s_driver_uninstall(I2S_NUM_0);
    i2sReady = i2sInstall();
}

void audioBeep(uint16_t /*freqHz*/, uint16_t /*durationMs*/) {
    if (!gSettings.soundEnabled) return;

    // Re-init if the driver was reset since last time.
    audioEnsureReady();
    if (!i2sReady) return;

    // Throttle: don't stack rapid click events.
    uint32_t now = millis();
    if (now - lastBeepMs < 120) return;
    lastBeepMs = now;

    initClickBuf();
    size_t written = 0;
    esp_err_t err = i2s_write(I2S_NUM_0, clickBuf, sizeof(clickBuf), &written, pdMS_TO_TICKS(20));
    if (err != ESP_OK) {
        // Driver went away again (e.g. BLE activity). Mark dirty for next call.
        Serial.printf("[audio] i2s_write err 0x%x, will reinit next call\n", err);
        i2sReady = false;
    }
}

void audioClickNav()  { if (gSettings.hapticClicks) audioBeep(1800, 10); }
void audioClickOk()   { audioBeep(1200, 20); }
void audioClickBack() { audioBeep(700,  20); }
