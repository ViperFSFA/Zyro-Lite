#pragma once
//  Pin map for the LilyGO T-Deck Plus.
//  Source: wiki.lilygo.cc/products/t-deck-series/t-deck-plus/

// Power
#define BOARD_POWERON         10   // HIGH when running on battery

// Shared SPI bus (display / SD / LoRa)
#define BOARD_SPI_SCK         40
#define BOARD_SPI_MOSI        41
#define BOARD_SPI_MISO        38

// Display (ST7789, 320x240)
#define BOARD_TFT_CS          12
#define BOARD_TFT_DC          11
#define BOARD_TFT_BACKLIGHT   42

// SD / TF card (SPI, shares bus above)
#define BOARD_SDCARD_CS       39

// LoRa SX1262 (868 MHz variant), shares SPI bus
#define RADIO_CS_PIN           9
#define RADIO_BUSY_PIN        13
#define RADIO_RST_PIN         17
#define RADIO_DIO1_PIN        45

// I2C bus (keyboard + misc sensors)
#define BOARD_I2C_SDA         18
#define BOARD_I2C_SCL          8
#define KEYBOARD_I2C_ADDR    0x55
#define BOARD_KEYBOARD_INT    46

// Trackball
#define BOARD_TBOX_UP           3   // G01
#define BOARD_TBOX_RIGHT        2   // G02
#define BOARD_TBOX_DOWN        15   // G03
#define BOARD_TBOX_LEFT         1   // G04
#define BOARD_TBOX_CLICK        0   // BOOT button / trackball center press
// NOTE: unavailable if microphone/I2S-RX in use

// Battery 
#define BOARD_BATTERY_ADC       4

// Speaker (generic I2S output, e.g. onboard amp) 
#define BOARD_I2S_WS             5
#define BOARD_I2S_BCK            7
#define BOARD_I2S_DOUT            6

//ES7210 mic array (I2S input) is not required for this build,
//defined for future use
#define BOARD_ES7210_MCLK       48
#define BOARD_ES7210_LRCK       21
#define BOARD_ES7210_SCK        47
#define BOARD_ES7210_DIN        14

// GPS (MIA-M10Q, T-Deck Plus only, UART)
#define BOARD_GPS_TX_PIN        43
#define BOARD_GPS_RX_PIN        44

// Misc
#define BOARD_TOUCH_INT         16  // unused

// --- GPIO app (external connector modules) ---
// UNCONFIGURED ON PURPOSE. Every pin on the T-Deck is already spoken for by
// something above (display/SD/LoRa SPI, I2C keyboard, trackball, I2S, GPS
// UART), and I don't have confirmed wiring for what you've actually got
// hooked up to the external connector. Set these to the real GPIO numbers
// your NRF24L01 / CC1101 modules are wired to before using those two modes -
// the GPIO app will refuse to talk to a module whose pins are still -1
// rather than guess and potentially collide with a pin already used above.
#define GPIO_NRF24_CE_PIN      -1
#define GPIO_NRF24_CSN_PIN     -1
// NRF24L01 SCK/MOSI/MISO share the main SPI bus (gSharedSPI) above.

#define GPIO_CC1101_CS_PIN     -1
#define GPIO_CC1101_GDO0_PIN   -1
#define GPIO_CC1101_GDO2_PIN   -1
// CC1101 SCK/MOSI/MISO also share the main SPI bus above.

// "Read GPIO" / "Custom" pin-toggle modes: the actual list of pins exposed
// on your external connector that are safe to use (not already claimed
// above). Empty by default - add the real numbers once you know them.
// Example: { 43, 44 }. Make sure they aren't double-booked with GPS.
static const int GPIO_CUSTOM_PINS[] = {};
#define GPIO_CUSTOM_PIN_COUNT (sizeof(GPIO_CUSTOM_PINS) / sizeof(GPIO_CUSTOM_PINS[0]))
