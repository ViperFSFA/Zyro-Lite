#pragma once
#include <Arduino.h>

// Usage:
//showLoadingOverlay("Scanning Wi-Fi...");
//your async operation here
//then, every tick(): poll your operation. once it's done: hideLoadingOverlay();
//and redraw your own screen with the result.

// loadingOverlayTick() just animates the spinner - call it every loop()
// iteration (main.cpp does this globally); it no-ops when not showing.
void showLoadingOverlay(const char *label);
void hideLoadingOverlay();
bool loadingOverlayActive();
void loadingOverlayTick();

// Alert overlay (warning_blink_64_64_28f) 
// A hard-stop "something's wrong" screen: blanks the entire screen (topbar
// included), shows the blinking warning icon top-middle with the message
// centered underneath it, and a visible countdown. Auto-dismisses after
// ALERT_DURATION_MS (5s), or immediately, the moment resolvedCheck() (if
// given) starts returning true, so a problem that clears itself (e.g. the SD
// card being reinserted) doesn't sit there bothering the user for the full
// 5 seconds.

// Because this display has no framebuffer, there's nothing to restore
// underneath automatically. onDismiss (if given) is called right as the
// alert clears so the caller can repaint whatever should be on screen again
// (e.g. rootMenuForceRedraw()).

// Usage:
//   showAlert("SD Card Removed", sdCardPresent, myRedrawFn);
//   // every loop() iteration:
//   alertTick();
//   // elsewhere, e.g. to skip normal input handling while it's up:
//   if (alertActive()) return;
void showAlert(const char *message, bool (*resolvedCheck)() = nullptr, void (*onDismiss)() = nullptr);
void alertTick();
bool alertActive();
