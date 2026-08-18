#pragma once
#include <Arduino.h>

// Plays a boot image from SD (preferring /boot.jpg, then /sd_card_files/boot.jpg)
// for SPLASH_DURATION_MS. If no boot image is available, it falls back to the
// existing /wallpaper.gif or the plain text splash. Blocking call by design
// (runs once at boot before the menu system starts).
void showSplash(bool sdOk);
