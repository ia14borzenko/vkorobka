#include "i80_lcd_bus.hpp"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_types.h"

#include "display_pins.hpp"
#include "ili9486_display.hpp"

static const char* TAG_I80 = "I80_LCD_BUS";

static esp_lcd_i80_bus_handle_t s_i80_bus = nullptr;
static esp_lcd_panel_io_handle_t s_io_handle = nullptr;

esp_lcd_panel_io_handle_t init_ili9486_i80_panel_io()
{
    if (s_io_handle) {
        return s_io_handle;
    }

    ESP_LOGI(TAG_I80, "Init I80 bus for ILI9486");

    esp_lcd_i80_bus_config_t bus_config = {};
    bus_config.dc_gpio_num = LCD_RS_GPIO;
    bus_config.wr_gpio_num = LCD_WR_GPIO;
    // Примечание: rd_gpio_num не поддерживается в esp_lcd_i80_bus_config_t,
    // поэтому пин RD нужно поднять вручную, как в старом бит‑бэнг драйвере,
    // чтобы контроллер не считал, что идёт цикл чтения.
    bus_config.clk_src     = LCD_CLK_SRC_DEFAULT;
    bus_config.bus_width   = 8;
    bus_config.max_transfer_bytes =
        Ili9486Display::WIDTH * Ili9486Display::HEIGHT * 2;

    bus_config.data_gpio_nums[0] = LCD_D0_GPIO;
    bus_config.data_gpio_nums[1] = LCD_D1_GPIO;
    bus_config.data_gpio_nums[2] = LCD_D2_GPIO;
    bus_config.data_gpio_nums[3] = LCD_D3_GPIO;
    bus_config.data_gpio_nums[4] = LCD_D4_GPIO;
    bus_config.data_gpio_nums[5] = LCD_D5_GPIO;
    bus_config.data_gpio_nums[6] = LCD_D6_GPIO;
    bus_config.data_gpio_nums[7] = LCD_D7_GPIO;

    // RD держим в HIGH, как делал Parallel8Bus, чтобы ILI9486 корректно принимал записи.
    gpio_config_t rd_conf{};
    rd_conf.mode = GPIO_MODE_OUTPUT;
    rd_conf.intr_type = GPIO_INTR_DISABLE;
    rd_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    rd_conf.pull_up_en   = GPIO_PULLUP_DISABLE;
    rd_conf.pin_bit_mask = 1ULL << LCD_RD_GPIO;
    ESP_ERROR_CHECK(gpio_config(&rd_conf));
    gpio_set_level(LCD_RD_GPIO, LCD_LEVEL_HIGH);

    ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_config, &s_i80_bus));

    esp_lcd_panel_io_i80_config_t io_config = {};
    io_config.cs_gpio_num = LCD_CS_GPIO;
    // Стартуем с более низкой частоты, чтобы уменьшить пиковое энергопотребление
    // и нагрузку на питание. Потом можно поднять до 10–20 МГц, когда всё будет стабильно.
    io_config.pclk_hz = 2 * 1000 * 1000; // 2 MHz
    io_config.trans_queue_depth = 5;
    io_config.lcd_cmd_bits   = 8;
    io_config.lcd_param_bits = 8;
    // Уровни RS (D/C), аналогичные старому бит-бэнг драйверу:
    // команда: RS = 0, данные: RS = 1.
    io_config.dc_levels.dc_idle_level  = 1;
    io_config.dc_levels.dc_cmd_level   = 0;
    io_config.dc_levels.dc_dummy_level = 0;
    io_config.dc_levels.dc_data_level  = 1;

    ESP_ERROR_CHECK(
        esp_lcd_new_panel_io_i80(s_i80_bus, &io_config, &s_io_handle));

    ESP_LOGI(TAG_I80, "I80 bus + IO initialized");
    return s_io_handle;
}

// Простейшая отладочная функция: пробуем прочитать пару регистров из ILI9486
// через аппаратный I80 и выводим результат в лог.
void i80_lcd_debug_read_id()
{
    if (!s_io_handle) {
        ESP_LOGW(TAG_I80, "debug_read_id: IO handle is null (bus not inited)");
        return;
    }

    esp_err_t err;

    // Читаем ID (команда 0x04: Read Display ID)
    uint8_t id_buf[4] = {0};
    err = esp_lcd_panel_io_rx_param(s_io_handle, 0x04, id_buf, sizeof(id_buf));
    if (err != ESP_OK) {
        ESP_LOGE(TAG_I80, "Read ID (0x04) failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG_I80,
                 "Read ID (0x04): %02X %02X %02X %02X",
                 id_buf[0], id_buf[1], id_buf[2], id_buf[3]);
    }

    // Читаем формат пикселя (0x3A) — должно быть 0x55 для RGB565
    uint8_t pixfmt = 0;
    err = esp_lcd_panel_io_rx_param(s_io_handle, 0x3A, &pixfmt, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_I80, "Read PIXFMT (0x3A) failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG_I80, "Read PIXFMT (0x3A): %02X", pixfmt);
    }

    // Читаем статус (0x0A) — просто для информации
    uint8_t status = 0;
    err = esp_lcd_panel_io_rx_param(s_io_handle, 0x0A, &status, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_I80, "Read STATUS (0x0A) failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG_I80, "Read STATUS (0x0A): %02X", status);
    }
}
