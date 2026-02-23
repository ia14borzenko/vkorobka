#include "message_queue_c_wrapper.h"
#include "message_queue.hpp"

extern "C" {

message_queue_handle_t message_queue_create(void)
{
    return reinterpret_cast<message_queue_handle_t>(new message_queue_t());
}

void message_queue_destroy(message_queue_handle_t handle)
{
    if (handle != nullptr)
    {
        delete reinterpret_cast<message_queue_t*>(handle);
    }
}

void message_queue_enqueue(message_queue_handle_t handle,
                           const msg_header_t* header,
                           const u8* payload,
                           u32 payload_len)
{
    if (handle == nullptr || header == nullptr)
    {
        return;
    }
    
    message_queue_t* queue = reinterpret_cast<message_queue_t*>(handle);
    queue->enqueue(*header, payload, payload_len);
}

int message_queue_dequeue(message_queue_handle_t handle,
                          msg_header_t* header_out,
                          u8* payload_out,
                          u32 max_payload_size,
                          u32* payload_len_out)
{
    if (handle == nullptr || header_out == nullptr)
    {
        return 0;
    }
    
    message_queue_t* queue = reinterpret_cast<message_queue_t*>(handle);
    
    u32 payload_len = 0;
    bool success = queue->dequeue(*header_out, payload_out, max_payload_size, &payload_len);
    
    if (success && payload_len_out != nullptr)
    {
        *payload_len_out = payload_len;
    }
    
    return success ? 1 : 0;
}

int message_queue_empty(message_queue_handle_t handle)
{
    if (handle == nullptr)
    {
        return 1; // Считаем пустым при ошибке
    }
    
    message_queue_t* queue = reinterpret_cast<message_queue_t*>(handle);
    return queue->empty() ? 1 : 0;
}

u32 message_queue_size(message_queue_handle_t handle)
{
    if (handle == nullptr)
    {
        return 0;
    }
    
    message_queue_t* queue = reinterpret_cast<message_queue_t*>(handle);
    return queue->size();
}

void message_queue_clear(message_queue_handle_t handle)
{
    if (handle == nullptr)
    {
        return;
    }
    
    message_queue_t* queue = reinterpret_cast<message_queue_t*>(handle);
    queue->clear();
}

} // extern "C"
