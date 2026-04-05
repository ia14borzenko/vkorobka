#include "message_bridge.hpp"
#include "tcp.hpp"
#include "esp_log.h"

static const char* TAG = "msg_bridge";

message_bridge_t::message_bridge_t()
    : tcp_transport_(nullptr)
{
    // Инициализируем обработчики как NULL
    for (int i = 0; i < 6; ++i)
    {
        type_handlers_[i] = nullptr;
        dest_handlers_[i] = nullptr;
    }
}

message_bridge_t::~message_bridge_t()
{
}

void message_bridge_t::init(tcp_t* tcp)
{
    tcp_transport_ = tcp;
}

bool message_bridge_t::route_from_tcp(const msg_header_t* header, const u8* payload, u32 payload_len)
{
    if (header == nullptr || !msg_validate_header(header))
    {
        ESP_LOGE(TAG, "Invalid message header from TCP");
        return false;
    }

    msg_destination_t dst = (msg_destination_t)header->destination_id;

    // Если сообщение для нас (ESP32) или BROADCAST, обрабатываем локально
    if (dst == MSG_DST_ESP32 || dst == MSG_DST_BROADCAST)
    {
        invoke_handlers(header, payload, payload_len);
    }

    return true;
}

void message_bridge_t::register_handler(msg_type_t msg_type, message_handler_t handler)
{
    if (msg_type >= MSG_TYPE_COMMAND && msg_type <= MSG_TYPE_ERROR)
    {
        type_handlers_[msg_type - 1] = handler;  // Индексация с 0
    }
}

void message_bridge_t::register_handler(msg_destination_t destination, message_handler_t handler)
{
    if (destination >= MSG_DST_WIN && destination <= MSG_DST_EXTERNAL)
    {
        dest_handlers_[destination - 1] = handler;  // Индексация с 0
    }
}

bool message_bridge_t::process_buffer(const u8* buffer, u32 buffer_len)
{
    if (buffer == nullptr || buffer_len < MSG_HEADER_LEN)
    {
        return false;
    }

    ESP_LOGD(TAG, "[DEBUG] process_buffer: buffer_len=%u", buffer_len);

    if (buffer_len >= MSG_HEADER_LEN)
    {
        char hex_buf[64];
        int hex_len = 0;
        for (int i = 0; i < MSG_HEADER_LEN && i < 12; i++)
        {
            hex_len += sprintf(hex_buf + hex_len, "%02X ", buffer[i]);
        }
        ESP_LOGD(TAG, "[DEBUG] process_buffer header bytes: %s", hex_buf);
        ESP_LOGD(TAG, "[DEBUG] process_buffer payload_len bytes [7-10]: %02X %02X %02X %02X", buffer[7],
                 buffer[8], buffer[9], buffer[10]);
    }

    msg_header_t header;
    const u8* payload = nullptr;
    u32 payload_len = 0;

    if (!msg_unpack(buffer, buffer_len, &header, &payload, &payload_len))
    {
        ESP_LOGE(TAG, "[DEBUG] process_buffer: msg_unpack failed!");
        return false;
    }

    ESP_LOGD(TAG, "[DEBUG] process_buffer unpacked: payload_len=%u, header.payload_len=%u, dst=%d",
             payload_len, header.payload_len, header.destination_id);
    
    return route_from_tcp(&header, payload, payload_len);
}

void message_bridge_t::invoke_handlers(const msg_header_t* header, const u8* payload, u32 payload_len)
{
    // Вызываем обработчик по типу
    if (header->msg_type >= MSG_TYPE_COMMAND && header->msg_type <= MSG_TYPE_ERROR)
    {
        message_handler_t handler = type_handlers_[header->msg_type - 1];
        if (handler != nullptr)
        {
            handler(header, payload, payload_len);
        }
    }

    // Вызываем обработчик по получателю
    if (header->destination_id >= MSG_DST_WIN && header->destination_id <= MSG_DST_EXTERNAL)
    {
        message_handler_t handler = dest_handlers_[header->destination_id - 1];
        if (handler != nullptr)
        {
            handler(header, payload, payload_len);
        }
    }
}

bool message_bridge_t::send_message(const msg_header_t& header, const u8* payload, u32 payload_len)
{
    msg_destination_t dst = (msg_destination_t)header.destination_id;
    
    // Если сообщение для Windows или внешнего приложения, отправляем через TCP
    if (dst == MSG_DST_WIN || dst == MSG_DST_EXTERNAL || dst == MSG_DST_BROADCAST)
    {
        if (tcp_transport_ && tcp_transport_->is_connected())
        {
            // Упаковываем сообщение в бинарный формат
            std::vector<u8> packet(MSG_HEADER_LEN + payload_len);
            u32 packed_size = msg_pack(&header, payload, payload_len, packet.data());
            
            if (packed_size > 0)
            {
                // Отправляем через TCP как raw данные
                return tcp_transport_->send(reinterpret_cast<const char*>(packet.data()), static_cast<int>(packed_size));
            }
        }
    }
    
    return false;
}
