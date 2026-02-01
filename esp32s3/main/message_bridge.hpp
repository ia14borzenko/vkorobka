#ifndef MESSAGE_BRIDGE_HPP
#define MESSAGE_BRIDGE_HPP

#include "message_protocol.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <functional>

// Forward declarations
class tcp_t;
class uart_bridge_t;

// Callback для обработки сообщений
typedef void (*message_handler_t)(const msg_header_t* header, const u8* payload, u32 payload_len);

class message_bridge_t
{
public:
    message_bridge_t();
    ~message_bridge_t();

    // Инициализация с указателями на транспортные слои
    void init(tcp_t* tcp, uart_bridge_t* uart);

    // Маршрутизация сообщения от TCP к UART
    bool route_from_tcp(const msg_header_t* header, const u8* payload, u32 payload_len);

    // Маршрутизация сообщения от UART к TCP
    bool route_from_uart(const msg_header_t* header, const u8* payload, u32 payload_len);

    // Регистрация обработчиков
    void register_handler(msg_type_t msg_type, message_handler_t handler);
    void register_handler(msg_destination_t destination, message_handler_t handler);

    // Обработка бинарного буфера
    bool process_buffer(const u8* buffer, u32 buffer_len, bool from_tcp);

    // Отправка сообщения через соответствующий транспорт
    bool send_message(const msg_header_t& header, const u8* payload, u32 payload_len);

private:
    tcp_t* tcp_transport_;
    uart_bridge_t* uart_transport_;
    
    // Обработчики (упрощенная версия для ESP32)
    message_handler_t type_handlers_[6];  // По типу сообщения
    message_handler_t dest_handlers_[6];  // По получателю
    
    void invoke_handlers(const msg_header_t* header, const u8* payload, u32 payload_len);
};

#endif // MESSAGE_BRIDGE_HPP
