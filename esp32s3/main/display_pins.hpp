#pragma once

#include "driver/gpio.h"

// D0..D7 → GPIO 4–11 (см. README.md)
constexpr gpio_num_t LCD_D0_GPIO = GPIO_NUM_4;
constexpr gpio_num_t LCD_D1_GPIO = GPIO_NUM_5;
constexpr gpio_num_t LCD_D2_GPIO = GPIO_NUM_6;
constexpr gpio_num_t LCD_D3_GPIO = GPIO_NUM_7;
constexpr gpio_num_t LCD_D4_GPIO = GPIO_NUM_8;
constexpr gpio_num_t LCD_D5_GPIO = GPIO_NUM_9;
constexpr gpio_num_t LCD_D6_GPIO = GPIO_NUM_10;
constexpr gpio_num_t LCD_D7_GPIO = GPIO_NUM_11;

// Управляющие сигналы
constexpr gpio_num_t LCD_RS_GPIO  = GPIO_NUM_12;
constexpr gpio_num_t LCD_CS_GPIO  = GPIO_NUM_13;
constexpr gpio_num_t LCD_RST_GPIO = GPIO_NUM_14;
constexpr gpio_num_t LCD_WR_GPIO  = GPIO_NUM_15;
constexpr gpio_num_t LCD_RD_GPIO  = GPIO_NUM_16;

// Уровни
constexpr int LCD_LEVEL_LOW  = 0;
constexpr int LCD_LEVEL_HIGH = 1;

