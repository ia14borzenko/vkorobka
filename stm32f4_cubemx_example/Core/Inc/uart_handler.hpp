#ifndef UART_HANDLER_HPP
#define UART_HANDLER_HPP

#include "message_protocol.h"
#include "usart.h"
#include <cstring>

#ifdef __cplusplus

// Callback для обработки принятых сообщений (C-совместимый указатель на функцию)
typedef void (*uart_message_handler_cb_t)(const msg_header_t* header, const u8* payload, u32 payload_len);

// Кольцевой буфер для RX данных
class ring_buffer_t
{
public:
    static constexpr u32 UART_RX_BUFFER_SIZE = 4096;
    
    ring_buffer_t() : head_(0), tail_(0), size_(0) 
    {
        memset(buffer_, 0, sizeof(buffer_));
    }
    
    bool push(const u8* data, u32 len)
    {
        if (len == 0 || data == nullptr) return false;
        
        for (u32 i = 0; i < len; ++i)
        {
            if (size_ >= UART_RX_BUFFER_SIZE)
            {
                // Буфер переполнен, пропускаем старые данные
                tail_ = (tail_ + 1) % UART_RX_BUFFER_SIZE;
                --size_;
            }
            
            buffer_[head_] = data[i];
            head_ = (head_ + 1) % UART_RX_BUFFER_SIZE;
            ++size_;
        }
        
        return true;
    }
    
    u32 pop(u8* out, u32 max_len)
    {
        if (out == nullptr || max_len == 0 || size_ == 0)
        {
            return 0;
        }
        
        u32 copied = 0;
        while (size_ > 0 && copied < max_len)
        {
            out[copied] = buffer_[tail_];
            tail_ = (tail_ + 1) % UART_RX_BUFFER_SIZE;
            --size_;
            ++copied;
        }
        
        return copied;
    }
    
    u32 peek(u8* out, u32 max_len) const
    {
        if (out == nullptr || max_len == 0 || size_ == 0)
        {
            return 0;
        }
        
        u32 copied = 0;
        u32 pos = tail_;
        u32 remaining = size_;
        
        while (remaining > 0 && copied < max_len)
        {
            out[copied] = buffer_[pos];
            pos = (pos + 1) % UART_RX_BUFFER_SIZE;
            --remaining;
            ++copied;
        }
        
        return copied;
    }
    
    u32 available() const { return size_; }
    
    void clear()
    {
        head_ = 0;
        tail_ = 0;
        size_ = 0;
    }
    
    const u8* data() const { return buffer_; }
    u32 size() const { return size_; }

private:
    u8 buffer_[UART_RX_BUFFER_SIZE];
    u32 head_;  // индекс записи
    u32 tail_;  // индекс чтения
    u32 size_;  // текущий размер
};

class uart_handler_t
{
public:
    uart_handler_t();
    ~uart_handler_t();

    // Инициализация UART (USART1 или USART2)
    bool init(int uart_num = 1, int baud_rate = 115200);

    // Запуск обработки
    bool start();

    // Остановка
    void stop();

    // Отправка бинарного сообщения
    bool send_message(const msg_header_t& header, const u8* payload, u32 payload_len);

    // Регистрация обработчика для разных типов сообщений
    void register_handler(msg_type_t msg_type, uart_message_handler_cb_t handler);
    void register_handler(msg_destination_t destination, uart_message_handler_cb_t handler);

    // Обработка входящих данных (вызывается из прерывания или основного цикла)
    void process_rx_data();

    // Проверка, инициализирован ли UART
    bool is_initialized() const { return initialized_; }

private:
    static constexpr int MAX_HANDLERS_PER_TYPE = 4;
    static constexpr int MAX_MSG_TYPES = 6;
    static constexpr int MAX_DESTINATIONS = 6;
    
    bool initialized_;
    int uart_num_;
    int baud_rate_;
    
    // Обработчики по типу сообщения
    uart_message_handler_cb_t type_handlers_[MAX_MSG_TYPES][MAX_HANDLERS_PER_TYPE];
    u32 type_handler_count_[MAX_MSG_TYPES];
    
    // Обработчики по получателю
    uart_message_handler_cb_t dest_handlers_[MAX_DESTINATIONS][MAX_HANDLERS_PER_TYPE];
    u32 dest_handler_count_[MAX_DESTINATIONS];
    
    ring_buffer_t rx_buffer_;
    
    void process_rx_buffer();
    void invoke_handlers(const msg_header_t& header, const u8* payload, u32 payload_len);
    
    // Низкоуровневые функции UART (используют HAL)
    int uart_read_bytes(u8* buffer, int max_len);
    int uart_write_bytes(const u8* buffer, int len);
    void uart_init_hw(int uart_num, int baud_rate);
    
    // Получить указатель на UART handle
    UART_HandleTypeDef* get_uart_handle();
};

#endif // __cplusplus

#endif // UART_HANDLER_HPP
