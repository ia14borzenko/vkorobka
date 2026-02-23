#pragma once

#include <cstdint>

// Режим ручного управления шиной (бит-бэнг через Parallel8Bus):
// если определить этот макрос, будет использоваться старый ручной драйвер вместо аппаратного I80.
// Это полезно для проверки физического подключения дисплея.
// #define USE_MANUAL_BUS

#ifdef USE_MANUAL_BUS
#include "parallel_bus.hpp"
#else
#include "esp_lcd_panel_io.h"
#endif

class Ili9486Display {
public:
    static constexpr uint16_t WIDTH  = 480;
    static constexpr uint16_t HEIGHT = 320;

#ifdef USE_MANUAL_BUS
    explicit Ili9486Display(Parallel8Bus& bus)
        : bus_(bus) {}
#else
    explicit Ili9486Display(esp_lcd_panel_io_handle_t io)
        : io_(io) {}
#endif

    void init();
    void setAddressWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
    void fillScreen(uint16_t color);
    // Заливка прямоугольника (для диагностики реальной карты координат)
    void fillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

private:
#ifdef USE_MANUAL_BUS
    Parallel8Bus& bus_;
#else
    esp_lcd_panel_io_handle_t io_;
#endif

    void writeColor(uint16_t rgb565);
};

