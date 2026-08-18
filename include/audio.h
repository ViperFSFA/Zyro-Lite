#pragma once
#include <Arduino.h>

void audioInit();
void audioBeep(uint16_t freqHz = 1200, uint16_t durationMs = 40);
void audioClickNav();  
void audioClickOk();    // confirm.
void audioClickBack(); 
