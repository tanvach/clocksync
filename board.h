//
// board.h - pin defaults, picked automatically from the board you compile for.
//
// You normally don't need to edit this: the radio pin can be changed from the
// web UI (from the SAFE_PINS list below). Edit only to add a board.
//
//   RADIO_PIN    default antenna output
//   MEAS_PIN     input for the frequency self-test ("f" command), -1 = none
//   BUTTON_PIN   active-low button, -1 = none (short press: TX on/off,
//                hold 3 s: open the Wi-Fi setup hotspot)
//   BUTTON_PULLUP  1 to enable the internal pull-up on BUTTON_PIN
//   LED_PIN      status LED, -1 = none
//   LED_RGB      1 = addressable RGB LED (WS2812/SK6812), 0 = plain LED
//   LED_ACTIVE_LOW 1 if a plain LED lights when the pin is LOW
//   SAFE_PINS    pins the web UI offers for the antenna
//
#pragma once

#if defined(ARDUINO_M5STACK_ATOM) || defined(ARDUINO_M5Stack_ATOM)
// M5Stack Atom Lite / Matrix / Echo
#define BOARD_NAME "M5Stack Atom"
#define RADIO_PIN 32
#define MEAS_PIN 33
#define BUTTON_PIN 39  // has an external pull-up
#define BUTTON_PULLUP 0
#define LED_PIN 27
#define LED_RGB 1
#define LED_ACTIVE_LOW 0
#define SAFE_PINS { 19, 21, 22, 23, 25, 26, 32, 33 }

#elif defined(ARDUINO_M5STACK_ATOMS3) || defined(ARDUINO_M5STACK_ATOMS3R)
// M5Stack AtomS3 / AtomS3 Lite / AtomS3R
#define BOARD_NAME "M5Stack AtomS3"
#define RADIO_PIN 5
#define MEAS_PIN 6
#define BUTTON_PIN 41
#define BUTTON_PULLUP 1
#ifdef PIN_RGB_LED
#define LED_PIN PIN_RGB_LED
#define LED_RGB 1
#else
#define LED_PIN -1
#define LED_RGB 0
#endif
#define LED_ACTIVE_LOW 0
#define SAFE_PINS { 1, 2, 5, 6, 7, 8, 38, 39 }

#elif CONFIG_IDF_TARGET_ESP32C3
// ESP32-C3 boards such as the "SuperMini" (blue LED on GPIO8, BOOT on GPIO9)
#define BOARD_NAME "ESP32-C3"
#define RADIO_PIN 6
#define MEAS_PIN 7
#define BUTTON_PIN 9
#define BUTTON_PULLUP 1
#define LED_PIN 8
#define LED_RGB 0
#define LED_ACTIVE_LOW 1
#define SAFE_PINS { 0, 1, 3, 4, 5, 6, 7, 10, 20, 21 }

#elif CONFIG_IDF_TARGET_ESP32S3
// ESP32-S3 DevKit boards (RGB LED on GPIO48 or 38, BOOT on GPIO0)
#define BOARD_NAME "ESP32-S3"
#define RADIO_PIN 4
#define MEAS_PIN 5
#define BUTTON_PIN 0
#define BUTTON_PULLUP 1
#ifdef PIN_RGB_LED
#define LED_PIN PIN_RGB_LED
#define LED_RGB 1
#else
#define LED_PIN -1
#define LED_RGB 0
#endif
#define LED_ACTIVE_LOW 0
#define SAFE_PINS { 1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 21 }

#elif CONFIG_IDF_TARGET_ESP32
// Classic ESP32 DevKit boards (blue LED on GPIO2, BOOT on GPIO0)
#define BOARD_NAME "ESP32"
#define RADIO_PIN 32
#define MEAS_PIN 33
#define BUTTON_PIN 0
#define BUTTON_PULLUP 1
#define LED_PIN 2
#define LED_RGB 0
#define LED_ACTIVE_LOW 0
#define SAFE_PINS { 4, 13, 14, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33 }

#else
// Other ESP32 variants: no LED or button, change the radio pin in the web UI.
#define BOARD_NAME "ESP32 (generic)"
#define RADIO_PIN 4
#define MEAS_PIN 5
#define BUTTON_PIN -1
#define BUTTON_PULLUP 0
#define LED_PIN -1
#define LED_RGB 0
#define LED_ACTIVE_LOW 0
#define SAFE_PINS { 4, 5, 6, 7 }
#endif
