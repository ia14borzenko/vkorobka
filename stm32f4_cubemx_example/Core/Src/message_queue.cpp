#include "message_queue.hpp"
#include "stm32f4xx.h"

message_queue_t::message_queue_t()
    : queue_size_(0)
    , primask_save_(0)
{
    // Инициализация очереди
    for (u32 i = 0; i < MAX_QUEUE_SIZE; ++i)
    {
        queue_[i] = queued_message_t();
    }
}

message_queue_t::~message_queue_t()
{
    clear();
}

void message_queue_t::enqueue(const msg_header_t& header, const u8* payload, u32 payload_len)
{
    if (payload_len > MAX_PAYLOAD_SIZE)
    {
        // Payload слишком большой, игнорируем
        return;
    }
    
    enter_critical();
    
    if (queue_size_ >= MAX_QUEUE_SIZE)
    {
        // Очередь переполнена, игнорируем новое сообщение
        exit_critical();
        return;
    }
    
    // Создаем новое сообщение
    queued_message_t new_msg(header, payload, payload_len);
    
    // Находим позицию для вставки по приоритету
    // Меньший priority = выше приоритет, должен быть ближе к началу
    u32 insert_pos = queue_size_;
    
    for (u32 i = 0; i < queue_size_; ++i)
    {
        if (has_higher_priority(new_msg, queue_[i]))
        {
            insert_pos = i;
            break;
        }
    }
    
    // Сдвигаем элементы вправо
    for (u32 i = queue_size_; i > insert_pos; --i)
    {
        queue_[i] = queue_[i - 1];
    }
    
    // Вставляем новое сообщение
    queue_[insert_pos] = new_msg;
    ++queue_size_;
    
    exit_critical();
}

bool message_queue_t::dequeue(msg_header_t& header, u8* payload_out, u32 max_payload_size, u32* payload_len_out)
{
    enter_critical();
    
    if (queue_size_ == 0)
    {
        exit_critical();
        return false;
    }
    
    // Берем первое сообщение (с наивысшим приоритетом)
    const queued_message_t& msg = queue_[0];
    header = msg.header;
    
    // Копируем payload
    u32 copy_len = msg.payload_len;
    if (copy_len > max_payload_size)
    {
        copy_len = max_payload_size;
    }
    
    if (payload_out != nullptr && copy_len > 0)
    {
        memcpy(payload_out, msg.payload, copy_len);
    }
    
    if (payload_len_out != nullptr)
    {
        *payload_len_out = msg.payload_len;
    }
    
    // Сдвигаем остальные элементы влево
    for (u32 i = 1; i < queue_size_; ++i)
    {
        queue_[i - 1] = queue_[i];
    }
    
    --queue_size_;
    
    // Очищаем последний элемент
    if (queue_size_ < MAX_QUEUE_SIZE)
    {
        queue_[queue_size_] = queued_message_t();
    }
    
    exit_critical();
    return true;
}

bool message_queue_t::empty() const
{
    // Для const метода нужно временно снять const
    message_queue_t* self = const_cast<message_queue_t*>(this);
    self->enter_critical();
    bool result = (queue_size_ == 0);
    self->exit_critical();
    return result;
}

u32 message_queue_t::size() const
{
    // Для const метода нужно временно снять const
    message_queue_t* self = const_cast<message_queue_t*>(this);
    self->enter_critical();
    u32 result = queue_size_;
    self->exit_critical();
    return result;
}

void message_queue_t::clear()
{
    enter_critical();
    
    for (u32 i = 0; i < queue_size_; ++i)
    {
        queue_[i] = queued_message_t();
    }
    
    queue_size_ = 0;
    
    exit_critical();
}

void message_queue_t::enter_critical()
{
    // Сохраняем состояние прерываний и отключаем их
    primask_save_ = __get_PRIMASK();
    __disable_irq();
}

void message_queue_t::exit_critical()
{
    // Восстанавливаем состояние прерываний
    __set_PRIMASK(primask_save_);
}
