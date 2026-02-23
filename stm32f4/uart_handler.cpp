#include "uart_handler.hpp"
#include <cstring>

// Заглушки для UART функций - нужно реализовать под конкретную платформу STM32
// Здесь используется общий подход, который нужно адаптировать под вашу платформу

uart_handler_t::uart_handler_t()
    : initialized_(false)
    , uart_num_(1)
    , baud_rate_(115200)
{
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

    // Инициализация аппаратного UART
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
    std::vector<u8> buffer(total_size);
    
    u32 packed_size = msg_pack(&header, payload, payload_len, buffer.data());
    if (packed_size == 0)
    {
        return false;
    }

    buffer.resize(packed_size);

    // Отправляем через UART
    int sent = uart_write_bytes(buffer.data(), static_cast<int>(buffer.size()));
    return sent == static_cast<int>(buffer.size());
}

void uart_handler_t::register_handler(msg_type_t msg_type, uart_message_handler_t handler)
{
    if (msg_type >= MSG_TYPE_COMMAND && msg_type <= MSG_TYPE_ERROR)
    {
        type_handlers_[msg_type - 1].push_back(handler);
    }
}

void uart_handler_t::register_handler(msg_destination_t destination, uart_message_handler_t handler)
{
    if (destination >= MSG_DST_WIN && destination <= MSG_DST_EXTERNAL)
    {
        dest_handlers_[destination - 1].push_back(handler);
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
        rx_buffer_.insert(rx_buffer_.end(), temp_buffer, temp_buffer + len);
        
        // Обрабатываем буфер
        process_rx_buffer();
    }
}

void uart_handler_t::process_rx_buffer()
{
    while (rx_buffer_.size() >= MSG_HEADER_LEN)
    {
        msg_header_t header;
        const u8* payload = nullptr;
        u32 payload_len = 0;

        if (!msg_unpack(rx_buffer_.data(), static_cast<u32>(rx_buffer_.size()), &header, &payload, &payload_len))
        {
            // Недостаточно данных или ошибка парсинга
            // Правило: если не получилось обработать заголовок - он сбрасывается без уведомления отправителю
            if (rx_buffer_.size() >= MSG_HEADER_LEN)
            {
                // Есть достаточно данных для заголовка, но он невалиден - сбрасываем буфер
                rx_buffer_.clear();
            }
            // Если данных недостаточно даже для заголовка - просто ждем
            break;
        }
        else
        {
            // Успешно распарсили сообщение
            u32 total_size = MSG_HEADER_LEN + payload_len;
            rx_buffer_.erase(rx_buffer_.begin(), rx_buffer_.begin() + total_size);

            // Вызываем обработчики
            invoke_handlers(header, payload, payload_len);
        }
    }
}

void uart_handler_t::invoke_handlers(const msg_header_t& header, const u8* payload, u32 payload_len)
{
    // Вызываем обработчики по типу
    if (header.msg_type >= MSG_TYPE_COMMAND && header.msg_type <= MSG_TYPE_ERROR)
    {
        for (auto& handler : type_handlers_[header.msg_type - 1])
        {
            if (handler)
            {
                handler(header, payload, payload_len);
            }
        }
    }

    // Вызываем обработчики по получателю
    if (header.destination_id >= MSG_DST_WIN && header.destination_id <= MSG_DST_EXTERNAL)
    {
        for (auto& handler : dest_handlers_[header.destination_id - 1])
        {
            if (handler)
            {
                handler(header, payload, payload_len);
            }
        }
    }
}

// Реализация низкоуровневых функций UART
// Нужно реализовать под конкретную платформу STM32 используя HAL или CubeMX

int uart_handler_t::uart_read_bytes(u8* buffer, int max_len)
{
    // Заглушка - нужно реализовать чтение из UART
    // Пример для STM32 HAL:
    // HAL_StatusTypeDef status = HAL_UART_Receive(&huart1, buffer, max_len, HAL_MAX_DELAY);
    // if (status == HAL_OK) return max_len;
    // return 0;
    
    // Или через DMA:
    // return HAL_UART_Receive_DMA(&huart1, buffer, max_len);
    
    (void)buffer;
    (void)max_len;
    return 0;
}

int uart_handler_t::uart_write_bytes(const u8* buffer, int len)
{
    // Заглушка - нужно реализовать запись в UART
    // Пример для STM32 HAL:
    // HAL_StatusTypeDef status = HAL_UART_Transmit(&huart1, (uint8_t*)buffer, len, HAL_MAX_DELAY);
    // if (status == HAL_OK) return len;
    // return 0;
    
    // Или через DMA:
    // return HAL_UART_Transmit_DMA(&huart1, (uint8_t*)buffer, len);
    
    (void)buffer;
    (void)len;
    return 0;
}

void uart_handler_t::uart_init_hw(int uart_num, int baud_rate)
{
    // Заглушка - нужно реализовать инициализацию UART
    // Пример для STM32 HAL (нужно настроить huart1 или huart2):
    // huart1.Instance = USART1;
    // huart1.Init.BaudRate = baud_rate;
    // huart1.Init.WordLength = UART_WORDLENGTH_8B;
    // huart1.Init.StopBits = UART_STOPBITS_1;
    // huart1.Init.Parity = UART_PARITY_NONE;
    // huart1.Init.Mode = UART_MODE_TX_RX;
    // huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    // HAL_UART_Init(&huart1);
    
    (void)uart_num;
    (void)baud_rate;
}
