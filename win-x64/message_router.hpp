#ifndef MESSAGE_ROUTER_HPP
#define MESSAGE_ROUTER_HPP

#include <functional>
#include <map>
#include <vector>
#include <mutex>
#include <string>
#include "message_protocol.h"

// Forward declarations
class tcp_t;
class udp_api_t;

// Callback для обработки сообщений
using message_handler_t = std::function<void(const msg_header_t& header, const u8* payload, u32 payload_len)>;

class message_router_t
{
public:
    message_router_t();
    ~message_router_t();

    // Регистрация обработчиков для разных типов сообщений
    void register_handler(msg_type_t msg_type, message_handler_t handler);
    void register_handler(msg_destination_t destination, message_handler_t handler);
    void register_handler(msg_type_t msg_type, msg_destination_t destination, message_handler_t handler);

    // Маршрутизация бинарного сообщения
    bool route_message(const msg_header_t& header, const u8* payload, u32 payload_len);

    // Маршрутизация из бинарного буфера
    bool route_from_buffer(const u8* buffer, u32 buffer_len);

    // Конвертация JSON → бинарный формат
    bool convert_json_to_binary(const std::string& json_str, std::vector<u8>& out_binary);

    // Конвертация бинарный → JSON формат
    bool convert_binary_to_json(const msg_header_t& header, const u8* payload, u32 payload_len, std::string& out_json, const std::string& test_id = "");

    // Установка ссылок на транспортные слои
    void set_tcp_transport(tcp_t* tcp);
    void set_udp_transport(udp_api_t* udp);

    // Отправка сообщения через соответствующий транспорт
    bool send_message(const msg_header_t& header, const u8* payload, u32 payload_len);

    // Получить следующий sequence number
    u8 get_next_sequence();

    // Парсинг JSON сообщения (публичный для использования в main.cpp)
    bool parse_json_message(const std::string& json_str, msg_header_t& header, std::vector<u8>& payload, std::string& test_id);

private:
    std::mutex mutex_;
    
    // Обработчики по типу сообщения
    std::map<msg_type_t, std::vector<message_handler_t>> type_handlers_;
    
    // Обработчики по получателю
    std::map<msg_destination_t, std::vector<message_handler_t>> destination_handlers_;
    
    // Обработчики по комбинации тип+получатель
    std::map<std::pair<msg_type_t, msg_destination_t>, std::vector<message_handler_t>> combined_handlers_;
    
    // Транспортные слои
    tcp_t* tcp_transport_;
    udp_api_t* udp_transport_;
    
    // Счетчик sequence
    u8 sequence_counter_;

    // Внутренние методы
    void invoke_handlers(const msg_header_t& header, const u8* payload, u32 payload_len);
    std::string create_json_message(const msg_header_t& header, const u8* payload, u32 payload_len, const std::string& test_id = "");
};

#endif // MESSAGE_ROUTER_HPP
