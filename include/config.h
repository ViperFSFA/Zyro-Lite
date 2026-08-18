#pragma once

#define FW_NAME              "Zyro-Lite"
#define FW_VERSION           "V1.0"

// RF (SX1262 LoRa) subsystem/firmware version shown on the Device Info page.
// This is a build-config constant, not something read back from the radio.
#define RF_HW_VERSION         "1.0"

// Display 
#define SCREEN_W              320
#define SCREEN_H              240

// Splash 
#define SPLASH_GIF_PATH       "/wallpaper.gif"
#define SPLASH_DURATION_MS    2000

// Menu animation 
#define HIGHLIGHT_ANIM_MS     140   // glide duration for the selection highlight
#define ROW_HEIGHT             34
#define TOPBAR_HEIGHT           20

// Menu row icon animations (bitmap, optional per-item) 
#define ICON_SRC_SIZE           64   // source bitmap width/height in pixels
#define ICON_FRAME_COUNT        28   // frames per animation (all current assets use 28)
#define ICON_RENDER_SIZE        24   // on-screen size the icon is downscaled to
#define ICON_FRAME_MS           90   // ms between animation frames while highlighted

#define ANIM_FRAME_MS           16

// Full-screen overlays (loading / alert) 
#define OVERLAY_SPINNER_SIZE     48  //globe
#define OVERLAY_SPINNER_FRAME_MS 80   

// Alert overlay (warning_blink_64_64_28f): a hard-stop "something's wrong"
// screen. Auto-dismisses after ALERT_DURATION_MS, or earlier the moment the
// caller's resolvedCheck() reports the problem is gone.
#define ALERT_ICON_SIZE          56   // on-screen warning icon size
#define ALERT_FRAME_MS           80   // ms between alert-icon animation frames
#define ALERT_DURATION_MS      5000   // max time an alert stays on screen

// Input 
#define TRACKBALL_DEBOUNCE_US 2500
#define KEY_REPEAT_MS          160
#define LONGPRESS_MS           500

// Free-roaming cursor mode (Settings > Trackball > Cursor): how many pixels
// the on-screen pointer (and the BLE Remote Control's HID mouse output)
// moves per detected trackball detent/edge.
#define CURSOR_STEP_PX            6
#define CURSOR_W                 11   // matches the lopaka-exported bitmap
#define CURSOR_H                 16

// Battery 
#define BATT_ADC_MIN_MV       3300  // ~0%
#define BATT_ADC_MAX_MV       4200  // ~100%
#define BATT_SAMPLE_MS         5000

// Settings persistence
#define PREFS_NAMESPACE       "zyro-prefs"

// Zyro-SDK (Apps > My Apps user .zApp loader)
#define ZAPP_DIR               "/apps"            // sandboxed root for installed apps on SD
#define ZAPP_REGISTRY_PATH     "/apps/registry.txt"
#define ZAPP_TEXT_MAX          (48 * 1024)          // max executable (.text) size per app, IRAM-backed
#define ZAPP_RODATA_DATA_MAX   (64 * 1024)          // max combined .rodata+.data+.bss size per app, PSRAM-backed
#define ZAPP_NAME_MAX           24
