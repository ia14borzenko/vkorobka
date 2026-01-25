#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>
#include <cstdio>  // sprintf ok здесь

static constexpr uart_port_t UART_PC = UART_NUM_0;
static constexpr uart_port_t UART_LINK = UART_NUM_1;
static constexpr int BAUDRATE_LINK = 115200;  // Тест
static constexpr int BAUDRATE_PC = 115200;

static void uart_init()
{
    uart_config_t cfg_link = {
        .baud_rate = BAUDRATE_LINK,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_APB,
        .flags = 0,
    };

    uart_driver_install(UART_LINK, 1024, 1024, 0, nullptr, 0);
    uart_param_config(UART_LINK, &cfg_link);
    uart_set_pin(UART_LINK, 17, 18, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    uart_config_t cfg_pc = {
        .baud_rate = BAUDRATE_PC,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_APB,
        .flags = 0,
    };

    uart_driver_install(UART_PC, 1024, 1024, 0, nullptr, 0);
    uart_param_config(UART_PC, &cfg_pc);

    uart_flush(UART_LINK);
    uart_flush(UART_PC);

    vTaskDelay(500 / portTICK_PERIOD_MS);
}

extern "C" void app_main()
{
    uart_init();

    const char* msg = "ESP32-S3 ready (hex debug v3)\r\n";
    uart_write_bytes(UART_PC, msg, strlen(msg));

    const char* test_msg = "Test from ESP to STM: 0xAA 0x55\r\n";
    uart_write_bytes(UART_LINK, test_msg, strlen(test_msg));

    uint8_t buf[128];
    char hex_buf[32];

    while (true)
    {
        // PC -> LINK
        int n = uart_read_bytes(UART_PC, buf, sizeof(buf), 100 / portTICK_PERIOD_MS);
        if (n > 0)
        {
            uart_write_bytes(UART_LINK, buf, n);
            for (int i = 0; i < n; i++) {
                sprintf(hex_buf, "TX to link: 0x%02X\r\n", buf[i]);
                uart_write_bytes(UART_PC, hex_buf, strlen(hex_buf));
            }
        }

        // LINK -> PC
        n = uart_read_bytes(UART_LINK, buf, sizeof(buf), 100 / portTICK_PERIOD_MS);
        if (n > 0)
        {
            for (int i = 0; i < n; i++) {
                sprintf(hex_buf, "RX from link: 0x%02X\r\n", buf[i]);
                uart_write_bytes(UART_PC, hex_buf, strlen(hex_buf));
            }
        }

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}