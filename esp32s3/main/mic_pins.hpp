#pragma once

#include "driver/gpio.h"

// INMP441 ↔ ESP32-S3 (см. README.md)
constexpr gpio_num_t MIC_I2S_GPIO_DIN  = GPIO_NUM_35;  // SD / DATA
constexpr gpio_num_t MIC_I2S_GPIO_WS   = GPIO_NUM_36;  // WS / LRCL
constexpr gpio_num_t MIC_I2S_GPIO_BCLK = GPIO_NUM_37;  // SCK / BCLK
