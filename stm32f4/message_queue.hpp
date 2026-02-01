#ifndef MESSAGE_QUEUE_HPP
#define MESSAGE_QUEUE_HPP

#include "message_protocol.h"
#include <vector>
#include <queue>
#include <functional>
#include <mutex>

// Структура сообщения в очереди
struct queued_message_t
{
    msg_header_t header;
    std::vector<u8> payload;
    
    queued_message_t() = default;
    queued_message_t(const msg_header_t& h, const u8* p, u32 len)
        : header(h)
    {
        if (p && len > 0)
        {
            payload.assign(p, p + len);
        }
    }
};

// Компаратор для приоритетной очереди (меньший приоритет = выше в очереди)
struct message_priority_compare
{
    bool operator()(const queued_message_t& a, const queued_message_t& b) const
    {
        // Меньший номер приоритета = выше приоритет
        return a.header.priority > b.header.priority;
    }
};

class message_queue_t
{
public:
    message_queue_t();
    ~message_queue_t();

    // Добавить сообщение в очередь
    void enqueue(const msg_header_t& header, const u8* payload, u32 payload_len);

    // Получить следующее сообщение из очереди (по приоритету)
    bool dequeue(msg_header_t& header, std::vector<u8>& payload);

    // Проверить, есть ли сообщения в очереди
    bool empty() const;

    // Получить размер очереди
    size_t size() const;

    // Очистить очередь
    void clear();

private:
    // Используем vector как контейнер для priority_queue
    std::priority_queue<queued_message_t, std::vector<queued_message_t>, message_priority_compare> queue_;
    mutable std::mutex mutex_;
};

#endif // MESSAGE_QUEUE_HPP
