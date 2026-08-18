#include "battery.h"
#include "pins.h"
#include "config.h"

static int lastPct = 100;
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
    uint32_t mv = analogReadMilliVolts(BOARD_BATTERY_ADC) * BATT_DIVIDER_RATIO;

    int pct = (int)(100.0f * (mv - BATT_ADC_MIN_MV) / (float)(BATT_ADC_MAX_MV - BATT_ADC_MIN_MV));
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    lastPct = pct;
    return pct;
}

bool batteryCharging() {
    return batteryPercent() >= 99;
}
