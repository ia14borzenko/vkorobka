#pragma once

#include <cstdint>
#include "parallel_bus.hpp"

class Ili9486Display {
public:
    static constexpr uint16_t WIDTH  = 480;
    static constexpr uint16_t HEIGHT = 320;

    explicit Ili9486Display(Parallel8Bus& bus)
        : bus_(bus) {}

    void init();
    void setAddressWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
    void fillScreen(uint16_t color);

private:
    Parallel8Bus& bus_;

    void writeColor(uint16_t rgb565);
};

