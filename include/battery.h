#pragma once
#include <Arduino.h>

void batteryInit();
int  batteryPercent(); // estimated 0-100 from the cell voltage curve
bool batteryCharging(); // false until charging can be verified by hardware

int batteryMillivolts();
