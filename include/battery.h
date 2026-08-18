#pragma once
#include <Arduino.h>

void batteryInit();
int  batteryPercent(); // 0-100
bool batteryCharging(); 
