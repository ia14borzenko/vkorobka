#ifndef MESSAGE_QUEUE_HPP
#define MESSAGE_QUEUE_HPP

#include "message_protocol.h"
#include <cstring>

#ifdef __cplusplus

// Максимальный размер payload в сообщении
#define MAX_PAYLOAD_SIZE 4096

// Структура сообщения в очереди
struct queued_message_t
{
    msg_header_t header;
    u8 payload[MAX_PAYLOAD_SIZE];
    u32 payload_len;
    
    queued_message_t()
        : payload_len(0)
    {
        memset(&header, 0, sizeof(header));
        memset(payload, 0, sizeof(payload));
    }
    
    queued_message_t(const msg_header_t& h, const u8* p, u32 len)
        : header(h)
        , payload_len(0)
    {
        if (p != nullptr && len > 0 && len <= MAX_PAYLOAD_SIZE)
        {
            memcpy(payload, p, len);
            payload_len = len;
        }
        else
        {
            memset(payload, 0, sizeof(payload));
        }
    }
};

class message_queue_t
{
public:
    message_queue_t();
    ~message_queue_t();

    // Добавить сообщение в очередь (по приоритету)
    void enqueue(const msg_header_t& header, const u8* payload, u32 payload_len);

    // Получить следующее сообщение из очереди (по приоритету)
    bool dequeue(msg_header_t& header, u8* payload_out, u32 max_payload_size, u32* payload_len_out);

    // Проверить, есть ли сообщения в очереди
    bool empty() const;

    // Получить размер очереди
    u32 size() const;

    // Очистить очередь
    void clear();

private:
    static constexpr u32 MAX_QUEUE_SIZE = 32;
    
    queued_message_t queue_[MAX_QUEUE_SIZE];
    u32 queue_size_;
    
    // Вспомогательная функция для сравнения приоритетов
    // Возвращает true, если a имеет более высокий приоритет (меньший номер)
    bool has_higher_priority(const queued_message_t& a, const queued_message_t& b) const
    {
        return a.header.priority < b.header.priority;
    }
    
    // Критическая секция: отключение/включение прерываний
    void enter_critical();
    void exit_critical();
    u32 primask_save_;
};

#endif // __cplusplus

#endif // MESSAGE_QUEUE_HPP
