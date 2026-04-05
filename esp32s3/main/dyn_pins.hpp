#pragma once

#include "driver/gpio.h"

// MAX98357A ↔ ESP32-S3 (см. README.md)
constexpr gpio_num_t DYN_I2S_GPIO_WS   = GPIO_NUM_39;  // LRCLK
constexpr gpio_num_t DYN_I2S_GPIO_BCLK = GPIO_NUM_40;  // BCLK
constexpr gpio_num_t DYN_I2S_GPIO_DOUT = GPIO_NUM_41; // DIN усилителя
constexpr gpio_num_t DYN_GPIO_SD_MODE  = GPIO_NUM_42; // SD_MODE (не shutdown = HIGH)
