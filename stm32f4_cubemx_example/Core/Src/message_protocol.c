#include "message_protocol.h"
#include <string.h>

u32 msg_pack(const msg_header_t* header, const void* payload, u32 payload_len, u8* out_buffer)
{
    if (header == NULL || out_buffer == NULL)
    {
        return 0;
    }
    
    // Проверка валидности заголовка
    if (!msg_validate_header(header))
    {
        return 0;
    }
    
    // Проверка размера payload
    if (payload_len > MSG_MAX_PAYLOAD_SIZE)
    {
        return 0;
    }
    
    // Проверка соответствия payload_len в заголовке
    if (header->payload_len != payload_len)
    {
        return 0;
    }
    
    u8* ptr = out_buffer;
    
    // Записываем заголовок байт за байтом в little-endian порядке
    ptr[0] = header->msg_type;
    ptr[1] = header->source_id;
    ptr[2] = header->destination_id;
    ptr[3] = header->route_flags;
    ptr[4] = header->priority;
    
    // Stream ID (2 bytes, little-endian)
    ptr[5] = (u8)(header->stream_id & 0xFF);
    ptr[6] = (u8)((header->stream_id >> 8) & 0xFF);
    
    // Payload length (4 bytes, little-endian)
    ptr[7] = (u8)(header->payload_len & 0xFF);
    ptr[8] = (u8)((header->payload_len >> 8) & 0xFF);
    ptr[9] = (u8)((header->payload_len >> 16) & 0xFF);
    ptr[10] = (u8)((header->payload_len >> 24) & 0xFF);
    
    // Sequence number (1 byte)
    ptr[11] = header->sequence;
    
    // Копируем payload если есть
    if (payload != NULL && payload_len > 0)
    {
        memcpy(ptr + MSG_HEADER_LEN, payload, payload_len);
    }
    
    return MSG_HEADER_LEN + payload_len;
}

int msg_unpack(const u8* rx_msg, u32 rx_msg_len, msg_header_t* out_header, 
               const u8** out_payload, u32* out_payload_len)
{
    if (rx_msg == NULL || out_header == NULL || out_payload == NULL || out_payload_len == NULL)
    {
        return 0;
    }
    
    // Проверяем минимальный размер
    if (rx_msg_len < MSG_HEADER_LEN)
    {
        return 0;
    }
    
    // Читаем заголовок
    out_header->msg_type = rx_msg[0];
    out_header->source_id = rx_msg[1];
    out_header->destination_id = rx_msg[2];
    out_header->route_flags = rx_msg[3];
    out_header->priority = rx_msg[4];
    
    // Stream ID (2 bytes, little-endian)
    out_header->stream_id = (u16)(rx_msg[5] | (rx_msg[6] << 8));
    
    // Payload length (4 bytes, little-endian)
    out_header->payload_len = (u32)(rx_msg[7] | (rx_msg[8] << 8) | 
                                    (rx_msg[9] << 16) | (rx_msg[10] << 24));
    
    // Sequence number (1 byte)
    out_header->sequence = rx_msg[11];
    
    // Проверяем валидность заголовка
    if (!msg_validate_header(out_header))
    {
        return 0;
    }
    
    // Проверяем, есть ли полный пакет
    u32 expected_total = MSG_HEADER_LEN + out_header->payload_len;
    if (rx_msg_len < expected_total)
    {
        // Недостаточно данных
        return 0;
    }
    
    // Устанавливаем указатель на payload
    *out_payload = (out_header->payload_len > 0) ? (rx_msg + MSG_HEADER_LEN) : NULL;
    *out_payload_len = out_header->payload_len;
    
    return 1;
}

int msg_validate_header(const msg_header_t* header)
{
    if (header == NULL)
    {
        return 0;
    }
    
    // Проверка типа сообщения
    if (header->msg_type < MSG_TYPE_COMMAND || header->msg_type > MSG_TYPE_ERROR)
    {
        return 0;
    }
    
    // Проверка source_id
    if (header->source_id < MSG_SRC_WIN || header->source_id > MSG_SRC_EXTERNAL)
    {
        return 0;
    }
    
    // Проверка destination_id (допускается BROADCAST = 0xFF)
    if (header->destination_id != MSG_DST_BROADCAST &&
        (header->destination_id < MSG_DST_WIN || header->destination_id > MSG_DST_EXTERNAL))
    {
        return 0;
    }
    
    // Проверка размера payload
    if (header->payload_len > MSG_MAX_PAYLOAD_SIZE)
    {
        return 0;
    }
    
    return 1;
}

msg_header_t msg_create_header(msg_type_t msg_type, msg_source_t source_id, 
                               msg_destination_t destination_id, u8 priority,
                               u16 stream_id, u32 payload_len, u8 sequence, u8 route_flags)
{
    msg_header_t header;
    
    header.msg_type = (u8)msg_type;
    header.source_id = (u8)source_id;
    header.destination_id = (u8)destination_id;
    header.priority = priority;
    header.stream_id = stream_id;
    header.payload_len = payload_len;
    header.sequence = sequence;
    header.route_flags = route_flags;
    
    return header;
}
