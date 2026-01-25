#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>

static constexpr uart_port_t UART_PC = UART_NUM_0;
static constexpr uart_port_t UART_LINK = UART_NUM_1;
static constexpr int BAUDRATE = 3000000;

static void uart_init()
{
    uart_config_t cfg {
        .baud_rate = BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_APB,
    .flags = 0                  ,
    };

    uart_driver_install(UART_LINK, 1024, 1024, 0, nullptr, 0);
    uart_param_config(UART_LINK, &cfg);
    uart_set_pin(UART_LINK, 17, 18, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

uart_config_t cfg_pc {
    .baud_rate = 115200,
    .data_bits = UART_DATA_8_BITS,
    .parity    = UART_PARITY_DISABLE,
    .stop_bits = UART_STOP_BITS_1,
    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    .rx_flow_ctrl_thresh = 0,
    .source_clk = UART_SCLK_APB,
    .flags = 0
};
    
    uart_driver_install(UART_PC, 1024, 1024, 0, nullptr, 0);
    uart_param_config(UART_PC, &cfg_pc);
}

extern "C" void app_main()
{
    uart_init();

    const char* msg = "ESP32-S3 ready\r\n";
    uart_write_bytes(UART_PC, msg, strlen(msg));

    uint8_t buf[128];

    while (true)
    {
        int n = uart_read_bytes(UART_PC, buf, sizeof(buf), 10 / portTICK_PERIOD_MS);
        if (n > 0)
        {
            uart_write_bytes(UART_LINK, buf, n);
        }

        n = uart_read_bytes(UART_LINK, buf, sizeof(buf), 10 / portTICK_PERIOD_MS);
        if (n > 0)
        {
            uart_write_bytes(UART_PC, buf, n);
        }
    }
}
