#pragma once

#include <cstdint>
#include "esp_lcd_panel_io.h"

class Ili9486Display {
public:
    static constexpr uint16_t WIDTH  = 480;
    static constexpr uint16_t HEIGHT = 320;

    explicit Ili9486Display(esp_lcd_panel_io_handle_t io)
        : io_(io) {}

    void init();
    void setAddressWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
    void fillScreen(uint16_t color);
    // Заливка прямоугольника (для диагностики реальной карты координат)
    void fillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
    // Тестовый паттерн: цветовые градиенты + полоса градаций серого
    void drawTestPattern();

private:
    esp_lcd_panel_io_handle_t io_;

    void writeColor(uint16_t rgb565);
};

