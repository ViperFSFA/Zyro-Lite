#include "battery.h"
#include "pins.h"
#include "config.h"
#include <algorithm>

static int filteredMv = 0, percentage = 0;
static uint32_t sampledAt = 0;
void batteryInit() {
    analogReadResolution(12);
    // Arduino-ESP32 3.x requires a registered channel before per-pin attenuation.
    analogRead(BOARD_BATTERY_ADC);
    analogSetPinAttenuation(BOARD_BATTERY_ADC, ADC_11db);
}
int batteryPercent() {
    if (filteredMv && millis() - sampledAt < BATT_SAMPLE_MS) return percentage;
    sampledAt = millis();
    int samples[15];
    for (int &v : samples) v = analogReadMilliVolts(BOARD_BATTERY_ADC) * 2;
    std::sort(samples, samples + 15);
    int mv = 0;
    for (int i = 4; i < 11; ++i) mv += samples[i];
    mv /= 7;
    filteredMv = filteredMv ? (filteredMv * 7 + mv) / 8 : mv;
    // Approximate resting single-cell Li-ion curve; not a fuel gauge.
    static const int volts[] = {3300,3500,3600,3700,3750,3800,3850,3900,4000,4100,4200};
    static const int levels[] = {0,5,10,20,30,40,50,60,75,90,100};
    int next = filteredMv >= 4200 ? 100 : 0;
    for (int i = 1; i < 11; ++i) {
        if (filteredMv < volts[i]) {
            next = levels[i-1] + (filteredMv-volts[i-1]) * (levels[i]-levels[i-1]) / (volts[i]-volts[i-1]);
            break;
        }
    }
    next = constrain(next, 0, 100);
    if (!percentage || abs(next - percentage) >= 2) percentage = next;
    return percentage;
}
int batteryMillivolts() { batteryPercent(); return filteredMv; }
// T-Deck exposes the cell ADC, but no verified charger-status GPIO.
// Voltage rises are NOT proof of charging (load recovery causes them too).
bool batteryCharging() { return false; }
