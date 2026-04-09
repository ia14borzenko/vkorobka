#pragma once

#include "sdkconfig.h"
#include "driver/gpio.h"

// MAX98357A pins.
// Use board mapping for ESP32-S3; keep a compile-safe fallback for other targets.
#if defined(CONFIG_IDF_TARGET_ESP32S3)
constexpr gpio_num_t DYN_I2S_GPIO_WS = GPIO_NUM_39;     // LRCLK
constexpr gpio_num_t DYN_I2S_GPIO_BCLK = GPIO_NUM_40;   // BCLK
constexpr gpio_num_t DYN_I2S_GPIO_DOUT = GPIO_NUM_41;   // DIN amplifier
constexpr gpio_num_t DYN_GPIO_SD_MODE = GPIO_NUM_42;    // SD_MODE (HIGH = run)
#else
constexpr gpio_num_t DYN_I2S_GPIO_WS = GPIO_NUM_18;     // LRCLK fallback
constexpr gpio_num_t DYN_I2S_GPIO_BCLK = GPIO_NUM_19;   // BCLK fallback
constexpr gpio_num_t DYN_I2S_GPIO_DOUT = GPIO_NUM_4;    // DIN amplifier fallback
constexpr gpio_num_t DYN_GPIO_SD_MODE = GPIO_NUM_5;     // SD_MODE fallback
#endif
