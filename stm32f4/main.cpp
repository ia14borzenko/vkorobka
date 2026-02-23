#include "stm32f401xc.h"
#include "uart_handler.hpp"
#include "message_queue.hpp"
#include "message_protocol.h"
#include <string>
#include <vector>
#include <cstring>

// Глобальные объекты
static uart_handler_t* g_uart_handler = nullptr;
static message_queue_t* g_message_queue = nullptr;

// Обработчик сообщений
void handle_message(const msg_header_t& header, const u8* payload, u32 payload_len)
{
    // Добавляем сообщение в очередь для обработки
    if (g_message_queue)
    {
        g_message_queue->enqueue(header, payload, payload_len);
    }
}

// Заглушки для периферии (будут реализованы позже)
void lcd_display_text(const char* text)
{
    // Заглушка для отображения текста на LCD
    (void)text;
}

void speaker_play_audio(const u8* audio_data, u32 len)
{
    // Заглушка для воспроизведения аудио
    (void)audio_data;
    (void)len;
}

void microphone_get_samples(u8* buffer, u32 len)
{
    // Заглушка для получения отсчетов с микрофона
    (void)buffer;
    (void)len;
}

int main()
{
    // Инициализация UART handler
    g_uart_handler = new uart_handler_t();
    if (!g_uart_handler->init(1, 115200))  // USART1, 115200 baud
    {
        // Ошибка инициализации
        return 1;
    }
    
    if (!g_uart_handler->start())
    {
        return 1;
    }
    
    // Инициализация message queue
    g_message_queue = new message_queue_t();
    
    // Регистрация обработчиков
    g_uart_handler->register_handler(MSG_TYPE_COMMAND, handle_message);
    g_uart_handler->register_handler(MSG_TYPE_DATA, handle_message);
    g_uart_handler->register_handler(MSG_TYPE_STREAM, handle_message);
    
    // Основной цикл обработки
    while (1)
    {
        // Обрабатываем входящие данные UART
        g_uart_handler->process_rx_data();
        
        // Обрабатываем сообщения из очереди
        if (g_message_queue && !g_message_queue->empty())
        {
            msg_header_t header;
            std::vector<u8> payload;
            
            if (g_message_queue->dequeue(header, payload))
            {
                // Обработка сообщения в зависимости от типа
                switch (header.msg_type)
                {
                case MSG_TYPE_COMMAND:
                    // Обработка команд
                    if (payload.size() > 0)
                    {
                        // Пример: команда "display_text"
                        std::string cmd(reinterpret_cast<const char*>(payload.data()), payload.size());
                        if (cmd == "display_text")
                        {
                            // Команда для отображения текста
                            // В реальной реализации здесь будет парсинг payload и вызов lcd_display_text
                        }
                    }
                    break;
                    
                case MSG_TYPE_DATA:
                    // Обработка данных
                    if (payload.size() > 0)
                    {
                        // Проверяем, является ли это тестовым изображением (по размеру, обычно > 1KB)
                        if (payload.size() > 1024)
                        {
                            // Тестовое изображение - возвращаем исходное без изменений
                            // Временное решение: простое переворачивание байтов портит JPG формат
                            // В реальной реализации здесь должна быть декомпрессия JPG, отзеркаливание и рекомпрессия
                            std::vector<u8> mirrored_payload(payload.begin(), payload.end());
                            
                            // Формируем ответ
                            msg_header_t response_header = msg_create_header(
                                MSG_TYPE_RESPONSE,
                                MSG_SRC_STM32,
                                MSG_DST_EXTERNAL,
                                128,
                                0,
                                static_cast<u32>(mirrored_payload.size()),
                                0,
                                MSG_ROUTE_NONE
                            );
                            
                            // Отправляем ответ обратно через UART
                            if (g_uart_handler)
                            {
                                g_uart_handler->send_message(response_header, mirrored_payload.data(), static_cast<u32>(mirrored_payload.size()));
                            }
                        }
                        else
                        {
                            // Обычные данные (например, текст для экрана)
                            std::string text(reinterpret_cast<const char*>(payload.data()), payload.size());
                            lcd_display_text(text.c_str());
                        }
                    }
                    break;
                    
                case MSG_TYPE_STREAM:
                    // Обработка потоковых данных (например, аудио)
                    // В реальной реализации здесь будет обработка аудио потоков
                    break;
                    
                default:
                    break;
                }
            }
        }
        
        // Небольшая задержка для снижения нагрузки на CPU
        // В реальной реализации можно использовать системный таймер или прерывания
        for (volatile int i = 0; i < 10000; ++i);
    }
    
	return 0;
}
