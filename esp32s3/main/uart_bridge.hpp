#ifndef UART_BRIDGE_HPP
#define UART_BRIDGE_HPP

#include "message_protocol.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include <vector>

// Callback для обработки принятых сообщений
typedef void (*uart_message_callback_t)(const msg_header_t* header, const u8* payload, u32 payload_len);

class uart_bridge_t
{
public:
    uart_bridge_t(uart_port_t uart_num = UART_NUM_2, int baud_rate = 115200);
    ~uart_bridge_t();

    // Инициализация UART
    bool init(int tx_pin = 17, int rx_pin = 16);

    // Запуск задач чтения/записи
    bool start();

    // Остановка
    void stop();

    // Отправка бинарного сообщения
    bool send_message(const msg_header_t* header, const u8* payload, u32 payload_len);

    // Установка callback для обработки входящих сообщений
    void set_message_callback(uart_message_callback_t callback);

    // Проверка, инициализирован ли UART
    bool is_initialized() const { return initialized_; }

private:
    uart_port_t uart_num_;
    int baud_rate_;
    bool initialized_;
    uart_message_callback_t rx_callback_;
    
    TaskHandle_t rx_task_handle_;
    TaskHandle_t tx_task_handle_;
    QueueHandle_t tx_queue_;
    QueueHandle_t uart_event_queue_;  // Очередь событий UART
    
    std::vector<u8> rx_buffer_;
    
    static constexpr int UART_RX_BUF_SIZE = 4096;
    static constexpr int UART_TX_BUF_SIZE = 2048;
    static constexpr int UART_QUEUE_SIZE = 10;
    
    // Внутренние методы
    static void uart_rx_task(void* pvParameters);
    static void uart_tx_task(void* pvParameters);
    void process_rx_buffer();
};

#endif // UART_BRIDGE_HPP
