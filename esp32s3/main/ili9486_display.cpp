#include "ili9486_display.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "display_pins.hpp"
#include <cstddef>
#include <cstdio> // для sprintf в логировании

static const char* TAG_LCD = "ILI9486";

#ifndef USE_MANUAL_BUS
#include "esp_lcd_panel_io.h"

// Тег оставляем, но подробные логи I80_TX по умолчанию отключаем,
// чтобы не засорять монитор. При необходимости можно будет включить
// обратно, раскомментировав логи внутри функций.
static const char* TAG_I80_TX = "I80_TX";

// Вспомогательная функция для логирования tx_param (сейчас делает ничего)
static void log_tx_param(esp_lcd_panel_io_handle_t /*io*/, uint8_t /*cmd*/,
                         const uint8_t* /*param*/, size_t /*param_size*/) {
    // Подробное логирование отключено.
}

// Вспомогательная функция для логирования tx_color (сейчас делает ничего)
static void log_tx_color(esp_lcd_panel_io_handle_t /*io*/, uint8_t /*cmd*/,
                         const void* /*color_data*/, size_t /*color_size*/) {
    // Подробное логирование отключено.
}
#endif

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
    // MADCTL (0x36): ориентация. Было 0x28, логические (0,0) попадали в физический нижний‑правый угол.
    // Ставим 0xE8 (поворот на 180°), чтобы (0,0) было в физическом левом‑верхнем.
    -1,   0x36, 0xE8,
    -1,   0xB1, 0xA0,0x1F,
    -1,   0x11,             // Sleep Out
    -1,   0x29               // Display ON
};

void Ili9486Display::init() {
    ESP_LOGI(TAG_LCD, "Display init start");

#ifdef USE_MANUAL_BUS
    // Ручной режим: используем старый Parallel8Bus
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
#else
    // Аппаратный I80 режим
    // Инициализируем и дергаем RST ножку отдельно от I80‑шины
    gpio_config_t rst_conf{};
    rst_conf.mode = GPIO_MODE_OUTPUT;
    rst_conf.intr_type = GPIO_INTR_DISABLE;
    rst_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    rst_conf.pull_up_en   = GPIO_PULLUP_DISABLE;
    rst_conf.pin_bit_mask = 1ULL << LCD_RST_GPIO;
    ESP_ERROR_CHECK(gpio_config(&rst_conf));

    gpio_set_level(LCD_RST_GPIO, LCD_LEVEL_LOW);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(LCD_RST_GPIO, LCD_LEVEL_HIGH);
    vTaskDelay(pdMS_TO_TICKS(120));

    const std::size_t n =
        sizeof(ili9486_init_cmds) / sizeof(ili9486_init_cmds[0]);
    std::size_t i = 0;

    while (i < n) {
        int16_t v = ili9486_init_cmds[i++];

        if (v == -1) {
            if (i >= n) {
                break;
            }
            v = ili9486_init_cmds[i++];
        }

        uint8_t cmd = static_cast<uint8_t>(v);

        // Собираем параметры текущей команды в небольшой временный буфер
        uint8_t params[16];
        std::size_t param_count = 0;

        while (i < n && ili9486_init_cmds[i] != -1) {
            if (param_count < sizeof(params)) {
                params[param_count++] =
                    static_cast<uint8_t>(ili9486_init_cmds[i]);
            }
            ++i;
        }

        log_tx_param(io_, cmd, param_count ? params : nullptr, param_count);
        esp_err_t err = esp_lcd_panel_io_tx_param(
            io_, cmd, param_count ? params : nullptr, param_count);
        if (err != ESP_OK) {
            ESP_LOGE(TAG_I80_TX, "TX_PARAM failed: cmd=0x%02X, err=%s", cmd, esp_err_to_name(err));
        }
        ESP_ERROR_CHECK(err);

        // Задержки после критических команд
        if (cmd == 0x11) {           // Sleep Out
            vTaskDelay(pdMS_TO_TICKS(120));
        } else if (cmd == 0x29) {    // Display ON
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        // Небольшая задержка после каждой команды для стабильности
        vTaskDelay(pdMS_TO_TICKS(1));
    }
#endif

    ESP_LOGI(TAG_LCD, "Display init done");
}

void Ili9486Display::setAddressWindow(uint16_t x0, uint16_t y0,
                                      uint16_t x1, uint16_t y1) {
#ifdef USE_MANUAL_BUS
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
    bus_.beginWriteCommand(0x2C);
#else
    // CASET (Column / X)
    uint8_t caset[4] = {
        static_cast<uint8_t>(x0 >> 8),
        static_cast<uint8_t>(x0 & 0xFF),
        static_cast<uint8_t>(x1 >> 8),
        static_cast<uint8_t>(x1 & 0xFF),
    };
    log_tx_param(io_, 0x2A, caset, sizeof(caset));
    esp_err_t err = esp_lcd_panel_io_tx_param(io_, 0x2A, caset, sizeof(caset));
    if (err != ESP_OK) {
        ESP_LOGE(TAG_I80_TX, "TX_PARAM failed: cmd=0x2A (CASET), err=%s", esp_err_to_name(err));
    }
    ESP_ERROR_CHECK(err);

    // PASET (Row / Y)
    uint8_t paset[4] = {
        static_cast<uint8_t>(y0 >> 8),
        static_cast<uint8_t>(y0 & 0xFF),
        static_cast<uint8_t>(y1 >> 8),
        static_cast<uint8_t>(y1 & 0xFF),
    };
    log_tx_param(io_, 0x2B, paset, sizeof(paset));
    err = esp_lcd_panel_io_tx_param(io_, 0x2B, paset, sizeof(paset));
    if (err != ESP_OK) {
        ESP_LOGE(TAG_I80_TX, "TX_PARAM failed: cmd=0x2B (PASET), err=%s", esp_err_to_name(err));
    }
    ESP_ERROR_CHECK(err);
#endif
}

void Ili9486Display::writeColor(uint16_t rgb565) {
#ifdef USE_MANUAL_BUS
    uint8_t hi = static_cast<uint8_t>(rgb565 >> 8);
    uint8_t lo = static_cast<uint8_t>(rgb565 & 0xFF);
    bus_.writeDataStream(hi);
    bus_.writeDataStream(lo);
#else
    // Отправка одного пикселя как цвета (используется при необходимости).
    // ВАЖНО: ILI9486 ожидает сначала старший байт, затем младший (big-endian).
    uint8_t buf[2] = {
        static_cast<uint8_t>(rgb565 >> 8),       // HI
        static_cast<uint8_t>(rgb565 & 0xFF)      // LO
    };
    log_tx_color(io_, 0x2C, buf, sizeof(buf));
    esp_err_t err = esp_lcd_panel_io_tx_color(io_, 0x2C, buf, sizeof(buf));
    if (err != ESP_OK) {
        ESP_LOGE(TAG_I80_TX, "TX_COLOR failed: cmd=0x2C, err=%s", esp_err_to_name(err));
    }
    ESP_ERROR_CHECK(err);
#endif
}

void Ili9486Display::fillScreen(uint16_t color) {
    // Полноэкранная заливка через fillRect, чтобы логика была единой.
    fillRect(0, 0, WIDTH, HEIGHT, color);
}

void Ili9486Display::fillRect(uint16_t x, uint16_t y,
                              uint16_t w, uint16_t h,
                              uint16_t color) {
    if (w == 0 || h == 0) {
        return;
    }

    uint16_t x1 = x + w - 1;
    uint16_t y1 = y + h - 1;
    if (x1 >= WIDTH)  x1 = WIDTH - 1;
    if (y1 >= HEIGHT) y1 = HEIGHT - 1;

    setAddressWindow(x, y, x1, y1);

#ifdef USE_MANUAL_BUS
    // Небольшая задержка после установки окна адресации перед началом записи данных
    vTaskDelay(pdMS_TO_TICKS(1));

    // Потоковая запись: setAddressWindow уже вызвал beginWriteCommand(0x2C),
    // так что CS уже низкий и RS уже установлен в HIGH. Просто пишем данные.
    uint8_t hi = static_cast<uint8_t>(color >> 8);
    uint8_t lo = static_cast<uint8_t>(color & 0xFF);

    uint32_t total =
        static_cast<uint32_t>(x1 - x + 1) * (y1 - y + 1);
    for (uint32_t i = 0; i < total; ++i) {
        bus_.writeDataStream(hi);
        bus_.writeDataStream(lo);
    }

    bus_.endWrite();
#else
    // Заливка прямоугольника одним цветом, построчно.
    // ILI9486 ожидает порядок байтов: HI, LO для каждого пикселя.
    const uint16_t width_pixels  = static_cast<uint16_t>(x1 - x + 1);
    const uint16_t height_pixels = static_cast<uint16_t>(y1 - y + 1);

    static uint8_t s_line_buf[Ili9486Display::WIDTH * 2]; // максимум по ширине
    for (uint16_t i = 0; i < width_pixels; ++i) {
        uint8_t hi = static_cast<uint8_t>(color >> 8);
        uint8_t lo = static_cast<uint8_t>(color & 0xFF);
        s_line_buf[2 * i]     = hi;
        s_line_buf[2 * i + 1] = lo;
    }

    ESP_LOGI(TAG_I80_TX,
             "TX_COLOR: fillRect start, x=%u y=%u w=%u h=%u",
             x, y, width_pixels, height_pixels);

    for (uint16_t yy = y; yy <= y1; ++yy) {
        // Устанавливаем окно на одну строку и шлём сразу всю линию.
        uint16_t y0_line = yy;
        uint16_t y1_line = yy;
        setAddressWindow(x, y0_line, x1, y1_line);

        esp_err_t err = esp_lcd_panel_io_tx_color(io_, 0x2C,
                                                  s_line_buf,
                                                  width_pixels * 2);
        if (err != ESP_OK) {
            ESP_LOGE(TAG_I80_TX, "TX_COLOR failed: y=%u, err=%s",
                     yy, esp_err_to_name(err));
        }
        ESP_ERROR_CHECK(err);
    }

    ESP_LOGI(TAG_I80_TX, "TX_COLOR: fillRect done");
#endif
}

