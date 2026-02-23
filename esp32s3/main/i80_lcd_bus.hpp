#pragma once

#include "esp_lcd_panel_io.h"

// Инициализирует аппаратную I80‑шину и слой panel IO для ILI9486
// Использует пины из display_pins.hpp (D0–D7, RS, WR, RD, CS).
// Возвращает singleton‑handle IO; повторные вызовы возвращают тот же дескриптор.
esp_lcd_panel_io_handle_t init_ili9486_i80_panel_io();

// Отладочная функция: пробует прочитать несколько регистров (ID, PIXFMT, STATUS)
// через аппаратный I80 и выводит результат в лог.
void i80_lcd_debug_read_id();
