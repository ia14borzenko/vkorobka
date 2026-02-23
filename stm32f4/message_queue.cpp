#include "message_queue.hpp"
#include <algorithm>

message_queue_t::message_queue_t()
{
}

message_queue_t::~message_queue_t()
{
    clear();
}

void message_queue_t::enqueue(const msg_header_t& header, const u8* payload, u32 payload_len)
{
    std::lock_guard<std::mutex> lock(mutex_);
    
    queued_message_t msg(header, payload, payload_len);
    
    // Для priority_queue нужно использовать vector как контейнер
    // Но так как мы используем кастомный контейнер, просто добавляем в очередь
    queue_.push(msg);
}

bool message_queue_t::dequeue(msg_header_t& header, std::vector<u8>& payload)
{
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (queue_.empty())
    {
        return false;
    }
    
    const queued_message_t& msg = queue_.top();
    header = msg.header;
    payload = msg.payload;
    
    queue_.pop();
    return true;
}

bool message_queue_t::empty() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty();
}

size_t message_queue_t::size() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

void message_queue_t::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Очищаем очередь
    while (!queue_.empty())
    {
        queue_.pop();
    }
}
