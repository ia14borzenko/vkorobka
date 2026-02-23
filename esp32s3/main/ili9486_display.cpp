#include "ili9486_display.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char* TAG_LCD = "ILI9486";

// Массив инициализации по образцу InitCmd[] из LCD_SCREEN.c (MAR3501)
// Каждая секция: [-1], CMD, далее параметры до следующего -1.
static const int16_t ili9486_init_cmds[] = {
    //  CMD    // параметры
    0xF1, 0x36,0x04,0x00,0x3C,0x0F,0x8F,
    -1,   0xF2, 0x18,0xA3,0x12,0x02,0x02,0x12,0xFF,0x10,0x00,
    -1,   0xF8, 0x21,0x04,
    -1,   0xF9, 0x00,0x08,
    -1,   0xB4, 0x00,
    -1,   0xC1, 0x41,
    -1,   0xC5, 0x00,0x91,0x80,0x00,
    -1,   0xE0, 0x0F,0x1F,0x1C,0x0C,0x0F,0x08,0x48,0x98,0x37,0x0A,0x13,0x04,0x11,0x0D,0x00,
    -1,   0xE1, 0x0F,0x32,0x2E,0x0B,0x0D,0x05,0x47,0x75,0x37,0x06,0x10,0x03,0x24,0x20,0x00,
    -1,   0x3A, 0x55,        // 16‑бит (RGB565) - ДОЛЖНО БЫТЬ ДО Sleep Out
    -1,   0x36, 0x28,        // ориентация
    -1,   0xB1, 0xA0,0x1F,
    -1,   0x11,             // Sleep Out
    -1,   0x29               // Display ON
};

void Ili9486Display::init() {
    ESP_LOGI(TAG_LCD, "Display init start");

    bus_.reset();
    vTaskDelay(pdMS_TO_TICKS(10)); // Небольшая задержка после reset

    const std::size_t n = sizeof(ili9486_init_cmds) / sizeof(ili9486_init_cmds[0]);
    std::size_t i = 0;

    while (i < n) {
        int16_t v = ili9486_init_cmds[i++];

        if (v == -1) {
            if (i >= n) break;
            v = ili9486_init_cmds[i++];
        }

        uint8_t cmd = static_cast<uint8_t>(v);
        bus_.writeCommand(cmd);

        // Параметры до следующего -1 или конца массива
        while (i < n && ili9486_init_cmds[i] != -1) {
            uint8_t p = static_cast<uint8_t>(ili9486_init_cmds[i++]);
            bus_.writeData(p);
        }

        // Задержки после критических команд
        if (cmd == 0x11) {           // Sleep Out
            vTaskDelay(pdMS_TO_TICKS(120));
        } else if (cmd == 0x29) {    // Display ON
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        // Небольшая задержка после каждой команды для стабильности
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    ESP_LOGI(TAG_LCD, "Display init done");
}

void Ili9486Display::setAddressWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    // CASET (Column / X) - сначала колонки
    bus_.writeCommand(0x2A);
    bus_.writeData(static_cast<uint8_t>(x0 >> 8));
    bus_.writeData(static_cast<uint8_t>(x0 & 0xFF));
    bus_.writeData(static_cast<uint8_t>(x1 >> 8));
    bus_.writeData(static_cast<uint8_t>(x1 & 0xFF));

    // PASET (Row / Y) - затем строки
    bus_.writeCommand(0x2B);
    bus_.writeData(static_cast<uint8_t>(y0 >> 8));
    bus_.writeData(static_cast<uint8_t>(y0 & 0xFF));
    bus_.writeData(static_cast<uint8_t>(y1 >> 8));
    bus_.writeData(static_cast<uint8_t>(y1 & 0xFF));

    // RAMWR - команда записи в память (CS останется низким для потоковой записи)
    // Используем beginWriteCommand чтобы CS остался низким
    bus_.beginWriteCommand(0x2C);
}

void Ili9486Display::writeColor(uint16_t rgb565) {
    uint8_t hi = static_cast<uint8_t>(rgb565 >> 8);
    uint8_t lo = static_cast<uint8_t>(rgb565 & 0xFF);
    bus_.writeData(hi);
    bus_.writeData(lo);
}

void Ili9486Display::fillScreen(uint16_t color) {
    setAddressWindow(0, 0, WIDTH - 1, HEIGHT - 1);
    
    // Небольшая задержка после установки окна адресации перед началом записи данных
    vTaskDelay(pdMS_TO_TICKS(1));
    
    // Потоковая запись: setAddressWindow уже вызвал beginWriteCommand(0x2C),
    // так что CS уже низкий и RS уже установлен в HIGH. Просто пишем данные.
    uint8_t hi = static_cast<uint8_t>(color >> 8);
    uint8_t lo = static_cast<uint8_t>(color & 0xFF);
    
    uint32_t total = static_cast<uint32_t>(WIDTH) * HEIGHT;
    for (uint32_t i = 0; i < total; ++i) {
        bus_.writeDataStream(hi);
        bus_.writeDataStream(lo);
    }
    
    bus_.endWrite();
}

