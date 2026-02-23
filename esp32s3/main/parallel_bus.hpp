#pragma once

#include <array>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "display_pins.hpp"

// Класс 8‑битной параллельной шины для MAR3501/ILI9486
class Parallel8Bus {
public:
    Parallel8Bus()
        : data_pins_{LCD_D0_GPIO, LCD_D1_GPIO, LCD_D2_GPIO, LCD_D3_GPIO,
                     LCD_D4_GPIO, LCD_D5_GPIO, LCD_D6_GPIO, LCD_D7_GPIO},
          rs_(LCD_RS_GPIO),
          cs_(LCD_CS_GPIO),
          rst_(LCD_RST_GPIO),
          wr_(LCD_WR_GPIO),
          rd_(LCD_RD_GPIO) {}

    esp_err_t init() {
        gpio_config_t io_conf{};
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en   = GPIO_PULLUP_DISABLE;

        // D0..D7
        uint64_t data_mask = 0;
        for (auto pin : data_pins_) {
            data_mask |= (1ULL << pin);
        }
        io_conf.pin_bit_mask = data_mask;
        ESP_ERROR_CHECK(gpio_config(&io_conf));

        // RS, CS, RST, WR, RD
        uint64_t ctrl_mask = (1ULL << rs_) | (1ULL << cs_) |
                             (1ULL << rst_) | (1ULL << wr_) |
                             (1ULL << rd_);
        io_conf.pin_bit_mask = ctrl_mask;
        ESP_ERROR_CHECK(gpio_config(&io_conf));

        // Безопасные уровни
        gpio_set_level(cs_,  LCD_LEVEL_HIGH); // чип не выбран
        gpio_set_level(wr_,  LCD_LEVEL_HIGH); // запись не активна
        gpio_set_level(rd_,  LCD_LEVEL_HIGH); // чтение не активно
        gpio_set_level(rs_,  LCD_LEVEL_HIGH); // данные
        gpio_set_level(rst_, LCD_LEVEL_HIGH); // не в сбросе

        return ESP_OK;
    }

    void reset() {
        gpio_set_level(rst_, LCD_LEVEL_LOW);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(rst_, LCD_LEVEL_HIGH);
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    // Запись одной команды (RS = 0)
    void writeCommand(uint8_t cmd) {
        gpio_set_level(rs_, LCD_LEVEL_LOW);
        select();
        writeByte(cmd);
        deselect();
        gpio_set_level(rs_, LCD_LEVEL_HIGH);
    }

    // Запись одного байта данных (RS = 1)
    void writeData(uint8_t data) {
        gpio_set_level(rs_, LCD_LEVEL_HIGH);
        select();
        writeByte(data);
        deselect();
    }

    // Для длинных последовательностей можно использовать begin/end.
    void beginWriteCommand(uint8_t cmd) {
        gpio_set_level(rs_, LCD_LEVEL_LOW);
        select();
        writeByte(cmd);
        gpio_set_level(rs_, LCD_LEVEL_HIGH);
    }

    void writeDataStream(uint8_t data) {
        writeByte(data);
    }

    void endWrite() {
        deselect();
    }

private:
    std::array<gpio_num_t, 8> data_pins_;
    gpio_num_t rs_;
    gpio_num_t cs_;
    gpio_num_t rst_;
    gpio_num_t wr_;
    gpio_num_t rd_;

    inline void select() {
        gpio_set_level(cs_, LCD_LEVEL_LOW);
    }

    inline void deselect() {
        gpio_set_level(cs_, LCD_LEVEL_HIGH);
    }

    inline void writeByte(uint8_t value) {
        // Выставляем биты на D0..D7
        for (std::size_t i = 0; i < data_pins_.size(); ++i) {
            int level = (value & (1U << i)) ? LCD_LEVEL_HIGH : LCD_LEVEL_LOW;
            gpio_set_level(data_pins_[i], level);
        }
        strobeWR();
    }

    inline void strobeWR() {
        gpio_set_level(wr_, LCD_LEVEL_LOW);
        // Небольшая задержка для обеспечения правильных таймингов
        // ESP32S3 работает на 160MHz, минимальная задержка ~6ns, но для надежности добавим небольшую паузу
        __asm__ __volatile__("nop; nop; nop; nop;");
        gpio_set_level(wr_, LCD_LEVEL_HIGH);
    }
};

