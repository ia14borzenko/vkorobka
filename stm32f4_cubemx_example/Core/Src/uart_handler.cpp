#include "uart_handler.hpp"
#include "usart.h"
#include <cstring>

uart_handler_t::uart_handler_t()
    : initialized_(false)
    , uart_num_(1)
    , baud_rate_(115200)
{
    // Инициализация массивов обработчиков
    for (int i = 0; i < MAX_MSG_TYPES; ++i)
    {
        type_handler_count_[i] = 0;
        for (int j = 0; j < MAX_HANDLERS_PER_TYPE; ++j)
        {
            type_handlers_[i][j] = nullptr;
        }
    }
    
    for (int i = 0; i < MAX_DESTINATIONS; ++i)
    {
        dest_handler_count_[i] = 0;
        for (int j = 0; j < MAX_HANDLERS_PER_TYPE; ++j)
        {
            dest_handlers_[i][j] = nullptr;
        }
    }
}

uart_handler_t::~uart_handler_t()
{
    stop();
}

bool uart_handler_t::init(int uart_num, int baud_rate)
{
    if (initialized_)
    {
        return true;
    }

    uart_num_ = uart_num;
    baud_rate_ = baud_rate;

    // Инициализация аппаратного UART (уже выполнена в CubeMX)
    uart_init_hw(uart_num, baud_rate);

    initialized_ = true;
    return true;
}

bool uart_handler_t::start()
{
    if (!initialized_)
    {
        return false;
    }

    return true;
}

void uart_handler_t::stop()
{
    if (initialized_)
    {
        initialized_ = false;
        rx_buffer_.clear();
    }
}

bool uart_handler_t::send_message(const msg_header_t& header, const u8* payload, u32 payload_len)
{
    if (!initialized_)
    {
        return false;
    }

    // Упаковываем сообщение
    u32 total_size = MSG_HEADER_LEN + payload_len;
    u8 buffer[MSG_HEADER_LEN + MSG_MAX_PAYLOAD_SIZE];
    
    if (total_size > sizeof(buffer))
    {
        return false;
    }
    
    u32 packed_size = msg_pack(&header, payload, payload_len, buffer);
    if (packed_size == 0)
    {
        return false;
    }

    // Отправляем через UART
    int sent = uart_write_bytes(buffer, static_cast<int>(packed_size));
    return sent == static_cast<int>(packed_size);
}

void uart_handler_t::register_handler(msg_type_t msg_type, uart_message_handler_cb_t handler)
{
    if (handler == nullptr)
    {
        return;
    }
    
    if (msg_type >= MSG_TYPE_COMMAND && msg_type <= MSG_TYPE_ERROR)
    {
        int idx = msg_type - 1;
        if (type_handler_count_[idx] < MAX_HANDLERS_PER_TYPE)
        {
            type_handlers_[idx][type_handler_count_[idx]] = handler;
            ++type_handler_count_[idx];
        }
    }
}

void uart_handler_t::register_handler(msg_destination_t destination, uart_message_handler_cb_t handler)
{
    if (handler == nullptr)
    {
        return;
    }
    
    if (destination >= MSG_DST_WIN && destination <= MSG_DST_EXTERNAL)
    {
        int idx = destination - 1;
        if (dest_handler_count_[idx] < MAX_HANDLERS_PER_TYPE)
        {
            dest_handlers_[idx][dest_handler_count_[idx]] = handler;
            ++dest_handler_count_[idx];
        }
    }
}

void uart_handler_t::process_rx_data()
{
    if (!initialized_)
    {
        return;
    }

    // Читаем доступные данные
    u8 temp_buffer[256];
    int len = uart_read_bytes(temp_buffer, sizeof(temp_buffer));
    
    if (len > 0)
    {
        // Добавляем в буфер
        rx_buffer_.push(temp_buffer, static_cast<u32>(len));
        
        // Обрабатываем буфер
        process_rx_buffer();
    }
}

void uart_handler_t::process_rx_buffer()
{
    while (rx_buffer_.available() >= MSG_HEADER_LEN)
    {
        // Пытаемся прочитать заголовок без удаления из буфера
        u8 header_buffer[MSG_HEADER_LEN];
        u32 peeked = rx_buffer_.peek(header_buffer, MSG_HEADER_LEN);
        
        if (peeked < MSG_HEADER_LEN)
        {
            break;
        }
        
//        msg_header_t header;
//        const u8* payload = nullptr;
//        u32 payload_len = 0;

        // Пытаемся распарсить заголовок
        msg_header_t header;
        const u8* payload = nullptr;
        u32 payload_len = 0;

        if (!msg_unpack(header_buffer, MSG_HEADER_LEN, &header, &payload, &payload_len))
        {
            // Недостаточно данных или ошибка парсинга
            // Если это не начало валидного сообщения, пропускаем байт
            if (rx_buffer_.available() > MSG_HEADER_LEN * 2)
            {
                u8 dummy;
                rx_buffer_.pop(&dummy, 1);
            }
            else
            {
                // Ждем больше данных
                break;
            }
        }
        else
        {
            // Проверяем, есть ли полный пакет
            u32 total_size = MSG_HEADER_LEN + payload_len;
            if (rx_buffer_.available() < total_size)
            {
                // Недостаточно данных для полного сообщения
                break;
            }
            
            // Проверяем размер payload
            if (payload_len > MSG_MAX_PAYLOAD_SIZE)
            {
                // Payload слишком большой, пропускаем сообщение
                u8 dummy;
                rx_buffer_.pop(&dummy, 1);
                continue;
            }
            
            // Читаем полное сообщение
            u8 full_message[MSG_HEADER_LEN + MSG_MAX_PAYLOAD_SIZE];
            if (total_size > sizeof(full_message))
            {
                // Сообщение слишком большое, пропускаем
                u8 dummy;
                rx_buffer_.pop(&dummy, 1);
                continue;
            }
            
            u32 read = rx_buffer_.pop(full_message, total_size);
            if (read != total_size)
            {
                break;
            }
            
            // Распаковываем еще раз для получения указателя на payload
            msg_header_t header_parsed;
            const u8* payload_ptr = nullptr;
            u32 payload_len_parsed = 0;
            
            if (msg_unpack(full_message, total_size, &header_parsed, &payload_ptr, &payload_len_parsed))
            {
                // Вызываем обработчики
                invoke_handlers(header_parsed, payload_ptr, payload_len_parsed);
            }
        }
    }
}

void uart_handler_t::invoke_handlers(const msg_header_t& header, const u8* payload, u32 payload_len)
{
    // Вызываем обработчики по типу
    if (header.msg_type >= MSG_TYPE_COMMAND && header.msg_type <= MSG_TYPE_ERROR)
    {
        int idx = header.msg_type - 1;
        for (u32 i = 0; i < type_handler_count_[idx]; ++i)
        {
            if (type_handlers_[idx][i] != nullptr)
            {
                type_handlers_[idx][i](&header, payload, payload_len);
            }
        }
    }

    // Вызываем обработчики по получателю
    if (header.destination_id >= MSG_DST_WIN && header.destination_id <= MSG_DST_EXTERNAL)
    {
        int idx = header.destination_id - 1;
        for (u32 i = 0; i < dest_handler_count_[idx]; ++i)
        {
            if (dest_handlers_[idx][i] != nullptr)
            {
                dest_handlers_[idx][i](&header, payload, payload_len);
            }
        }
    }
}

// Реализация низкоуровневых функций UART с использованием HAL
int uart_handler_t::uart_read_bytes(u8* buffer, int max_len)
{
    if (buffer == nullptr || max_len <= 0)
    {
        return 0;
    }
    
    UART_HandleTypeDef* huart = get_uart_handle();
    if (huart == nullptr)
    {
        return 0;
    }
    
    // Проверяем, есть ли данные для чтения
    if (__HAL_UART_GET_FLAG(huart, UART_FLAG_RXNE) == RESET)
    {
        return 0;
    }
    
    // Читаем доступные данные (неблокирующий режим)
    HAL_StatusTypeDef status = HAL_UART_Receive(huart, buffer, max_len, 0);
    if (status == HAL_OK)
    {
        return max_len;
    }
    else if (status == HAL_TIMEOUT)
    {
        // Читаем сколько получилось
        u32 available = 0;
        while (available < static_cast<u32>(max_len) && 
               __HAL_UART_GET_FLAG(huart, UART_FLAG_RXNE) != RESET)
        {
            buffer[available] = (u8)(huart->Instance->DR & 0xFF);
            ++available;
        }
        return static_cast<int>(available);
    }
    
    return 0;
}

int uart_handler_t::uart_write_bytes(const u8* buffer, int len)
{
    if (buffer == nullptr || len <= 0)
    {
        return 0;
    }
    
    UART_HandleTypeDef* huart = get_uart_handle();
    if (huart == nullptr)
    {
        return 0;
    }
    
    HAL_StatusTypeDef status = HAL_UART_Transmit(huart, (uint8_t*)buffer, len, HAL_MAX_DELAY);
    if (status == HAL_OK)
    {
        return len;
    }
    
    return 0;
}

void uart_handler_t::uart_init_hw(int uart_num, int baud_rate)
{
    // Инициализация уже выполнена в CubeMX через MX_USART1_UART_Init() и MX_USART2_UART_Init()
    // Здесь можно обновить baud rate, если нужно
    (void)uart_num;
    (void)baud_rate;
}

UART_HandleTypeDef* uart_handler_t::get_uart_handle()
{
    extern UART_HandleTypeDef huart1;
    extern UART_HandleTypeDef huart2;
    
    if (uart_num_ == 1)
    {
        return &huart1;
    }
    else if (uart_num_ == 2)
    {
        return &huart2;
    }
    
    return nullptr;
}
