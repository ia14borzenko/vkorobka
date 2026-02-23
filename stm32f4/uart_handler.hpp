#ifndef UART_HANDLER_HPP
#define UART_HANDLER_HPP

#include "message_protocol.h"
#include <functional>
#include <vector>

// Callback для обработки принятых сообщений
using uart_message_handler_t = std::function<void(const msg_header_t& header, const u8* payload, u32 payload_len)>;

class uart_handler_t
{
public:
    uart_handler_t();
    ~uart_handler_t();

    // Инициализация UART (USART1 или USART2)
    bool init(int uart_num = 1, int baud_rate = 115200);

    // Запуск обработки
    bool start();

    // Остановка
    void stop();

    // Отправка бинарного сообщения
    bool send_message(const msg_header_t& header, const u8* payload, u32 payload_len);

    // Регистрация обработчика для разных типов сообщений
    void register_handler(msg_type_t msg_type, uart_message_handler_t handler);
    void register_handler(msg_destination_t destination, uart_message_handler_t handler);

    // Обработка входящих данных (вызывается из прерывания или основного цикла)
    void process_rx_data();

    // Проверка, инициализирован ли UART
    bool is_initialized() const { return initialized_; }

private:
    bool initialized_;
    int uart_num_;
    int baud_rate_;
    
    std::vector<uart_message_handler_t> type_handlers_[6];  // По типу сообщения
    std::vector<uart_message_handler_t> dest_handlers_[6];  // По получателю
    
    std::vector<u8> rx_buffer_;
    
    static constexpr int UART_RX_BUFFER_SIZE = 4096;
    
    void process_rx_buffer();
    void invoke_handlers(const msg_header_t& header, const u8* payload, u32 payload_len);
    
    // Низкоуровневые функции UART (заглушки, нужно реализовать под конкретную платформу)
    int uart_read_bytes(u8* buffer, int max_len);
    int uart_write_bytes(const u8* buffer, int len);
    void uart_init_hw(int uart_num, int baud_rate);
};

#endif // UART_HANDLER_HPP
