#include "battery.h"
#include "pins.h"
#include "config.h"

static int lastPct = 100;
static int lastMv = 4200;
static int trendMv = 0;
static int chargeTrendScore = 0;
static bool chargingDetected = false;
static uint32_t lastSampleMs = 0;

void batteryInit() {
    analogReadResolution(12);
    analogSetPinAttenuation(BOARD_BATTERY_ADC, ADC_11db);
}

int batteryPercent() {
    uint32_t now = millis();
    if (now - lastSampleMs < BATT_SAMPLE_MS && lastSampleMs != 0) {
        return lastPct;
    }
    lastSampleMs = now;

    // NOTE: board uses a resistor divider on the ADC input; factor of 2 is the
    // common ratio but verify with a multimeter and adjust
    // BATT_DIVIDER_RATIO if readings are off.
    const float BATT_DIVIDER_RATIO = 2.0f;
    // Average several ADC reads. A single ESP32 ADC reading can jump enough
    // to make both the percentage and charging UI visibly twitch.
    uint32_t adcMv = 0;
    for (int i = 0; i < 8; i++) adcMv += analogReadMilliVolts(BOARD_BATTERY_ADC);
    uint32_t mv = (adcMv / 8) * BATT_DIVIDER_RATIO;

    if (trendMv == 0) {
        trendMv = (int)mv;
    } else {
        int delta = (int)mv - trendMv;
        if (delta >= 3) chargeTrendScore = min(8, chargeTrendScore + 2);
        else if (delta <= -3) chargeTrendScore = max(-8, chargeTrendScore - 3);
        else if (chargeTrendScore > 0) chargeTrendScore--;
        else if (chargeTrendScore < 0) chargeTrendScore++;

        trendMv = (trendMv * 3 + (int)mv) / 4;
        if (chargeTrendScore >= 4 && trendMv < BATT_ADC_MAX_MV - 15) chargingDetected = true;
        if (chargeTrendScore <= -3 || trendMv >= BATT_ADC_MAX_MV - 10) chargingDetected = false;
    }
    lastMv = trendMv;

    int pct = (int)(100.0f * (lastMv - BATT_ADC_MIN_MV) / (float)(BATT_ADC_MAX_MV - BATT_ADC_MIN_MV));
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    lastPct = pct;
    return pct;
}

bool batteryCharging() {
    batteryPercent(); // refreshes the trend when the sample interval elapsed
    return chargingDetected;
}
