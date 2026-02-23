#ifndef MESSAGE_BRIDGE_HPP
#define MESSAGE_BRIDGE_HPP

#include "message_protocol.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <functional>

// Forward declaration TCP-транспорта
class tcp_t;

// Callback для обработки сообщений
typedef void (*message_handler_t)(const msg_header_t* header, const u8* payload, u32 payload_len);

class message_bridge_t
{
public:
    message_bridge_t();
    ~message_bridge_t();

    // Инициализация с указателем на TCP-транспорт
    void init(tcp_t* tcp);

    // Маршрутизация сообщения, пришедшего по TCP
    bool route_from_tcp(const msg_header_t* header, const u8* payload, u32 payload_len);

    // Регистрация обработчиков
    void register_handler(msg_type_t msg_type, message_handler_t handler);
    void register_handler(msg_destination_t destination, message_handler_t handler);

    // Обработка бинарного буфера, принятого по TCP
    bool process_buffer(const u8* buffer, u32 buffer_len);

    // Отправка сообщения через соответствующий транспорт
    bool send_message(const msg_header_t& header, const u8* payload, u32 payload_len);

private:
    tcp_t* tcp_transport_;
    
    // Обработчики (упрощенная версия для ESP32)
    message_handler_t type_handlers_[6];  // По типу сообщения
    message_handler_t dest_handlers_[6];  // По получателю
    
    void invoke_handlers(const msg_header_t* header, const u8* payload, u32 payload_len);
};

#endif // MESSAGE_BRIDGE_HPP
