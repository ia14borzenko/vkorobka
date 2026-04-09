
#include <ws2tcpip.h>

#include <iostream>
#include <thread>
#include <atomic>
#include <string>
#include <mutex>
#include <condition_variable>
#include <chrono>

#include "sys.hpp"
#include "netpack.h"
#include "message_protocol.h"
#include "image_processor.hpp"


char rx_payload[10240];

static help_t help_cmd;
static send_t send_cmd;
static status_t status_cmd;
static test_esp32_t test_esp32_cmd;

static std::atomic<bool> g_running{ true };

// Глобальные указатели на TCP и WiFi объекты для доступа из console_thread
static tcp_t* g_tcp_ptr = nullptr;
static wifi_t* g_wifi_ptr = nullptr;

// Глобальные объекты для нового протокола
message_router_t* g_message_router = nullptr;  // Убрано static для доступа из tcp.cpp
static udp_api_t* g_udp_api = nullptr;

// Хранилище для test_id и адреса клиента для ответов
static std::map<std::string, std::pair<std::string, int>> g_test_clients;
static std::mutex g_test_clients_mutex;

// Хранилище соответствия между destination (ESP32/STM32) и test_id для отслеживания запросов
static std::map<msg_destination_t, std::string> g_destination_to_test_id;
static std::mutex g_destination_to_test_id_mutex;

void console_thread()
{
    std::cout << "'help' for command list\n" << ANSI_ENDL;

    while (g_running.load())
    {
        std::cout << "> ";

        std::string line;
        if (!std::getline(std::cin, line))
        {
            g_running.store(false);
            break;
        }

        std::string cmd_name;
        cctx_t ctx;
        ctx.tcp = g_tcp_ptr;  // Передаем указатель на TCP объект
        ctx.wifi = g_wifi_ptr;  // Передаем указатель на WiFi объект
        ctx.message_router = g_message_router;  // Передаем указатель на message_router

        parse_line(line, cmd_name, ctx);

        if (cmd_name.empty())
            continue;

        auto& reg = cvar_t::registry();
        auto it = reg.find(cmd_name);
        if (it == reg.end())
        {
            std::cout << ANSI_WARN << "unknown command" << ANSI_ENDL << std::endl;
            continue;
        }

        it->second->exec(ctx);
    }
}

static tcp_t tcp(&g_running);
static wifi_t wifi(&g_running);
static message_router_t message_router;
static udp_api_t udp_api(&g_running, 1236);

// Глобальные переменные для синхронизации с командой test_esp32
std::mutex g_response_mutex;
std::condition_variable g_response_cv;
std::atomic<bool> g_test_response_received{ false };
std::atomic<bool> g_status_response_received{ false };
std::string g_esp32_status_response;

// Обработчик принятых пакетов (старый формат - больше не используется, оставлен для совместимости)
void handle_packet(cmdcode_t cmd_code, const char* payload, u32 payload_len)
{
    // Старый CMD протокол больше не используется
    // Все сообщения должны приходить через новый протокол
    std::cout << ANSI_WARN << "[app] Legacy CMD packet received (ignored):" << ANSI_ENDL;
    std::cout << "  CMD Code: 0x" << std::hex << cmd_code << std::dec << std::endl;
    std::cout << "  Payload length: " << payload_len << " bytes" << std::endl;
    std::cout << "  Note: All communication should use the new message_protocol" << ANSI_ENDL << std::endl;
}

// Обработчик новых сообщений через message_router
void handle_new_message(const msg_header_t& header, const u8* payload, u32 payload_len)
{
    std::cout << ANSI_INFO << "[app] [HANDLE] New protocol message received:" << ANSI_ENDL;
    std::cout << "  Type: " << static_cast<int>(header.msg_type) << std::endl;
    std::cout << "  Source: " << static_cast<int>(header.source_id) 
              << (header.source_id == MSG_SRC_ESP32 ? " (ESP32)" : 
                  header.source_id == MSG_SRC_WIN ? " (WIN)" :
                  header.source_id == MSG_SRC_EXTERNAL ? " (EXTERNAL)" : " (LEGACY_STM32)") << std::endl;
    std::cout << "  Destination: " << static_cast<int>(header.destination_id) 
              << (header.destination_id == MSG_DST_EXTERNAL ? " (EXTERNAL)" : 
                  header.destination_id == MSG_DST_WIN ? " (WIN)" :
                  header.destination_id == MSG_DST_ESP32 ? " (ESP32)" : " (LEGACY_STM32)") << std::endl;
    std::cout << "  Priority: " << static_cast<int>(header.priority) << std::endl;
    std::cout << "  Payload length: " << header.payload_len << " bytes" << std::endl;

    // Поток PCM с микрофона (ESP32 -> EXTERNAL), не путать с RESPONSE / LCD
    if (header.source_id == MSG_SRC_ESP32 && header.msg_type == MSG_TYPE_STREAM &&
        header.destination_id == MSG_DST_EXTERNAL && payload && payload_len > 0 && g_udp_api && g_message_router)
    {
        std::string test_id;
        std::string client_ip;
        int client_port = 0;
        {
            std::lock_guard<std::mutex> lock(g_destination_to_test_id_mutex);
            auto it = g_destination_to_test_id.find(MSG_DST_ESP32);
            if (it != g_destination_to_test_id.end())
                test_id = it->second;
        }
        if (!test_id.empty())
        {
            std::lock_guard<std::mutex> lock(g_test_clients_mutex);
            auto it = g_test_clients.find(test_id);
            if (it != g_test_clients.end())
            {
                client_ip = it->second.first;
                client_port = it->second.second;
            }
        }
        std::string json_str;
        if (g_message_router->convert_binary_to_json(header, payload, payload_len, json_str, test_id))
        {
            if (!client_ip.empty() && client_port > 0)
                g_udp_api->send_json_to(client_ip, client_port, json_str);
            else
                g_udp_api->send_json_to_all(json_str);
            std::cout << ANSI_SUCC << "[app] ESP32 STREAM forwarded to UDP (e.g. mic PCM)" << ANSI_ENDL << std::endl;
        }
        return;
    }
    
        // Обработка ответов от ESP32 (включая изображения)
        if (header.source_id == MSG_SRC_ESP32 && 
            header.msg_type == MSG_TYPE_RESPONSE && payload && payload_len > 0)
        {
            const char* component_name = "ESP32";
            msg_destination_t dst_key = MSG_DST_ESP32;
            
        std::cout << ANSI_INFO << "[app] [HANDLE] Processing response from " << component_name 
                  << ", payload_len=" << payload_len << ANSI_ENDL;
        
            // ВАЖНО для ИИ-агента: Определение типа ответа для правильной обработки test_id
            // CHUNK_ACK - промежуточные подтверждения, test_id НЕ должен очищаться
            // LCD_FRAME_OK - финальный ответ, test_id должен быть очищен после отправки
            // Это критически важно, иначе финальный ответ придёт без test_id и Python его не обработает
            bool is_chunk_ack = false;
            bool is_final_response = false;
            std::string payload_str;
            
            if (payload_len < 1000)  // Текстовые ответы обычно маленькие
            {
                payload_str = std::string(reinterpret_cast<const char*>(payload), payload_len);
                
                // Проверяем, является ли это CHUNK_ACK (не очищаем test_id для него)
                // CHUNK_ACK используется для flow control - Python ждёт подтверждения перед отправкой следующего чанка
                if (payload_str.find("CHUNK_ACK:") == 0 || payload_str.find("CHUNK_ACK_AUDIO:") == 0)
                {
                    is_chunk_ack = true;
                    std::cout << ANSI_INFO << "[app] [HANDLE] This is a CHUNK_ACK, keeping test_id mapping" << ANSI_ENDL;
                }
                // Проверяем, является ли это финальным ответом (LCD_FRAME_OK)
                // LCD_FRAME_OK отправляется после завершения всех чанков и вывода всего кадра
                else if (payload_str.find("LCD_FRAME_OK") == 0)
                {
                    is_final_response = true;
                    std::cout << ANSI_INFO << "[app] [HANDLE] This is LCD_FRAME_OK final response" << ANSI_ENDL;
                }
            
            // Проверяем, является ли это ответом на тест
            if (payload_str == "TEST_RESPONSE")
            {
                std::lock_guard<std::mutex> lock(g_response_mutex);
                g_test_response_received.store(true);
                g_response_cv.notify_one();
                std::cout << ANSI_SUCC << "[app] Test response received from " << component_name << ANSI_ENDL << std::endl;
                return;
            }
            
            // Проверяем, является ли это ответом со статусом
            if (payload_str.substr(0, 7) == "STATUS:")
            {
                std::lock_guard<std::mutex> lock(g_response_mutex);
                g_esp32_status_response = payload_str;
                g_status_response_received.store(true);
                g_response_cv.notify_one();
                std::cout << ANSI_SUCC << "[app] Status response received from " << component_name << ANSI_ENDL << std::endl;
                return;
            }
        }
        
        // Если это не текстовый ответ, возможно это изображение - пересылаем в Python
        std::cout << ANSI_INFO << "[app] [HANDLE] " << component_name 
                  << " response appears to be binary data (possibly image), forwarding to Python client" << ANSI_ENDL;
        
        // Ищем test_id для этого ответа по source_id
        std::string test_id;
        std::string client_ip;
        int client_port = 0;
        
        // Ищем test_id по destination (ESP32 -> MSG_DST_ESP32)
        {
            std::lock_guard<std::mutex> lock(g_destination_to_test_id_mutex);
            auto it = g_destination_to_test_id.find(dst_key);
            if (it != g_destination_to_test_id.end())
            {
                test_id = it->second;
                std::cout << ANSI_INFO << "[app] [HANDLE] Found test_id=" << test_id 
                          << " for " << component_name << " response" << ANSI_ENDL;
            }
            else
            {
                std::cout << ANSI_WARN << "[app] [HANDLE] No test_id found for " << component_name << " response" << ANSI_ENDL;
            }
        }
        
        // Ищем адрес клиента по test_id
        if (!test_id.empty())
        {
            std::lock_guard<std::mutex> lock(g_test_clients_mutex);
            auto it = g_test_clients.find(test_id);
            if (it != g_test_clients.end())
            {
                client_ip = it->second.first;
                client_port = it->second.second;
                std::cout << ANSI_INFO << "[app] [HANDLE] Found client " << client_ip << ":" << client_port 
                          << " for test_id=" << test_id << ANSI_ENDL;
            }
            else
            {
                std::cout << ANSI_WARN << "[app] [HANDLE] No client found for test_id=" << test_id << ANSI_ENDL;
            }
        }
        
        // Конвертируем ответ в JSON и отправляем в Python
        if (g_udp_api && g_message_router)
        {
            std::string json_str;
            if (g_message_router->convert_binary_to_json(header, payload, payload_len, json_str, test_id))
            {
                if (!client_ip.empty() && client_port > 0)
                {
                    g_udp_api->send_json_to(client_ip, client_port, json_str);
                    std::cout << ANSI_SUCC << "[app] [HANDLE] " << component_name 
                              << " response forwarded to " << client_ip << ":" << client_port 
                              << " with test_id=" << test_id << ANSI_ENDL;
                }
                else
                {
                    g_udp_api->send_json_to_all(json_str);
                    std::cout << ANSI_SUCC << "[app] [HANDLE] " << component_name 
                              << " response forwarded to all clients (fallback) with test_id=" << test_id << ANSI_ENDL;
                }
                
                // ВАЖНО для ИИ-агента: Очистка test_id только для финальных ответов
                // CHUNK_ACK - это промежуточные подтверждения, test_id НЕ должен очищаться
                //   (иначе финальный LCD_FRAME_OK придёт без test_id и Python его не обработает)
                // LCD_FRAME_OK - это финальный ответ, после него test_id можно очистить
                // 
                // Раньше test_id очищался после каждого ответа, что приводило к потере test_id
                // для финального ответа, если он приходил после последнего CHUNK_ACK
                if (!test_id.empty() && !is_chunk_ack)
                {
                    {
                        std::lock_guard<std::mutex> lock(g_destination_to_test_id_mutex);
                        g_destination_to_test_id.erase(dst_key);
                        std::cout << ANSI_INFO << "[app] [HANDLE] Cleared test_id mapping for " << component_name 
                                  << (is_final_response ? " (final response)" : "") << ANSI_ENDL;
                    }
                    // Не удаляем из g_test_clients, так как клиент может использовать тот же test_id для других запросов
                }
                else if (is_chunk_ack)
                {
                    std::cout << ANSI_INFO << "[app] [HANDLE] Keeping test_id mapping for CHUNK_ACK" << ANSI_ENDL;
                }
            }
            else
            {
                std::cout << ANSI_ERR << "[app] [HANDLE] Failed to convert " << component_name << " response to JSON" << ANSI_ENDL;
            }
        }
        return;
    }
    
    // Обработка тестовых сообщений с изображениями для WIN
    if (header.destination_id == MSG_DST_WIN && header.msg_type == MSG_TYPE_DATA && payload && payload_len > 0)
    {
        std::cout << ANSI_INFO << "[app] Processing image test for WIN component" << ANSI_ENDL;
        
        // Отзеркаливаем изображение
        std::vector<u8> input_image(payload, payload + payload_len);
        std::vector<u8> mirrored_image;
        
        if (mirror_jpeg_horizontal(input_image, mirrored_image))
        {
            std::cout << ANSI_SUCC << "[app] Image mirrored successfully: " 
                      << input_image.size() << " -> " << mirrored_image.size() << " bytes" << ANSI_ENDL;
            
            // Формируем ответ
            msg_header_t response_header = msg_create_header(
                MSG_TYPE_RESPONSE,
                MSG_SRC_WIN,
                MSG_DST_EXTERNAL,
                128,
                0,
                static_cast<u32>(mirrored_image.size()),
                0,
                MSG_ROUTE_NONE
            );
            
            // Отправляем ответ через UDP на адрес клиента
            // Ищем test_id по destination (WIN)
            if (g_udp_api && g_message_router)
            {
                std::string json_str;
                std::string test_id;
                std::string client_ip;
                int client_port = 0;
                
                // Ищем test_id по destination (WIN)
                {
                    std::lock_guard<std::mutex> lock(g_destination_to_test_id_mutex);
                    auto it = g_destination_to_test_id.find(MSG_DST_WIN);
                    if (it != g_destination_to_test_id.end())
                    {
                        test_id = it->second;
                        std::cout << ANSI_INFO << "[app] Found test_id=" << test_id 
                                  << " for WIN response" << ANSI_ENDL;
                    }
                    else
                    {
                        std::cout << ANSI_WARN << "[app] No test_id found for WIN response" << ANSI_ENDL;
                    }
                }
                
                // Ищем адрес клиента по test_id
                if (!test_id.empty())
                {
                    std::lock_guard<std::mutex> lock(g_test_clients_mutex);
                    auto it = g_test_clients.find(test_id);
                    if (it != g_test_clients.end())
                    {
                        client_ip = it->second.first;
                        client_port = it->second.second;
                        std::cout << ANSI_INFO << "[app] Found client " << client_ip << ":" << client_port 
                                  << " for test_id=" << test_id << ANSI_ENDL;
                    }
                    else
                    {
                        std::cout << ANSI_WARN << "[app] No client found for test_id=" << test_id << ANSI_ENDL;
                    }
                }
                
                // Используем test_id при создании JSON
                if (g_message_router->convert_binary_to_json(response_header, mirrored_image.data(), 
                                                              static_cast<u32>(mirrored_image.size()), json_str, test_id))
                {
                    // Отправляем на конкретный адрес если известен
                    if (!client_ip.empty() && client_port > 0)
                    {
                        g_udp_api->send_json_to(client_ip, client_port, json_str);
                        std::cout << ANSI_SUCC << "[app] Response sent to " << client_ip << ":" << client_port 
                                  << " с test_id=" << test_id << ANSI_ENDL;
                    }
                    else
                    {
                        // Fallback: отправляем всем
                        g_udp_api->send_json_to_all(json_str);
                        std::cout << ANSI_SUCC << "[app] Response sent to all clients (fallback) с test_id=" << test_id << ANSI_ENDL;
                    }
                    
                    // Очищаем test_id после успешной отправки ответа
                    if (!test_id.empty())
                    {
                        std::lock_guard<std::mutex> lock(g_destination_to_test_id_mutex);
                        g_destination_to_test_id.erase(MSG_DST_WIN);
                        std::cout << ANSI_INFO << "[app] Cleared test_id mapping for WIN" << ANSI_ENDL;
                    }
                }
                else
                {
                    std::cout << ANSI_ERR << "[app] Failed to convert binary to JSON" << ANSI_ENDL << std::endl;
                }
            }
        }
        else
        {
            std::cout << ANSI_ERR << "[app] Failed to mirror image" << ANSI_ENDL;
        }
    }
    
    // Если сообщение для внешних приложений (ответ от ESP32 или STM32), отправляем через UDP
    if (header.destination_id == MSG_DST_EXTERNAL && header.msg_type == MSG_TYPE_RESPONSE && g_udp_api)
    {
        // Ищем test_id в последнем полученном сообщении для отправки на правильный адрес
        std::string test_id;
        std::string client_ip;
        int client_port = 0;
        
        {
            std::lock_guard<std::mutex> lock(g_test_clients_mutex);
            // Берем последний зарегистрированный клиент (упрощенная версия)
            if (!g_test_clients.empty())
            {
                auto it = g_test_clients.rbegin(); // Последний элемент
                test_id = it->first;
                client_ip = it->second.first;
                client_port = it->second.second;
                std::cout << ANSI_INFO << "[app] Используем test_id=" << test_id 
                          << " для ответа клиенту " << client_ip << ":" << client_port << ANSI_ENDL << std::endl;
            }
            else
            {
                std::cout << ANSI_WARN << "[app] Нет зарегистрированных клиентов для ответа" << ANSI_ENDL << std::endl;
            }
        }
        
        std::string json_str;
        if (g_message_router && g_message_router->convert_binary_to_json(header, payload, payload_len, json_str, test_id))
        {
            // Отправляем на конкретный адрес если известен
            if (!client_ip.empty() && client_port > 0)
            {
                g_udp_api->send_json_to(client_ip, client_port, json_str);
                std::cout << ANSI_SUCC << "[app] Response from " 
                          << (header.source_id == MSG_SRC_ESP32 ? "ESP32" : 
                              header.source_id == MSG_SRC_STM32 ? "STM32" : "unknown")
                          << " forwarded to " << client_ip << ":" << client_port 
                          << " с test_id=" << test_id << ANSI_ENDL;
            }
            else
            {
                // Fallback: отправляем всем
                g_udp_api->send_json_to_all(json_str);
                std::cout << ANSI_SUCC << "[app] Response forwarded to all clients (fallback) с test_id=" << test_id << ANSI_ENDL;
            }
        }
    }
}

// Обработчик UDP JSON сообщений
void handle_udp_json(const std::string& json_str, const std::string& client_ip, int client_port)
{
    std::cout << ANSI_INFO << "[udp] JSON message from " << client_ip << ":" << client_port << ANSI_ENDL << std::endl;
    
    if (g_message_router)
    {
        // Извлекаем test_id из JSON для сохранения адреса клиента
        // Используем parse_json_message для правильного извлечения test_id
        msg_header_t dummy_header;
        std::vector<u8> dummy_payload;
        std::string test_id;
        
        if (g_message_router)
        {
            // Парсим JSON для извлечения test_id (используем временный объект)
            std::string json_copy = json_str;
            if (g_message_router->parse_json_message(json_copy, dummy_header, dummy_payload, test_id))
            {
                if (!test_id.empty())
                {
                    // Сохраняем адрес клиента для ответа
                    std::lock_guard<std::mutex> lock(g_test_clients_mutex);
                    g_test_clients[test_id] = std::make_pair(client_ip, client_port);
                    std::cout << ANSI_INFO << "[udp] Сохранен test_id=" << test_id 
                              << " для клиента " << client_ip << ":" << client_port << ANSI_ENDL << std::endl;
                }
                else
                {
                    std::cout << ANSI_WARN << "[udp] test_id пустой в JSON сообщении" << ANSI_ENDL << std::endl;
                }
            }
            else
            {
                std::cout << ANSI_WARN << "[udp] Не удалось распарсить JSON для извлечения test_id" << ANSI_ENDL << std::endl;
            }
        }
        
        // Конвертируем JSON в бинарный формат и маршрутизируем
        std::vector<u8> binary_data;
        if (g_message_router->convert_json_to_binary(json_str, binary_data))
        {
            // Сохраняем соответствие между destination и test_id для последующего использования
            if (!test_id.empty())
            {
                msg_header_t temp_header;
                std::vector<u8> temp_payload;
                std::string temp_test_id;
                if (g_message_router->parse_json_message(json_str, temp_header, temp_payload, temp_test_id))
                {
                    msg_destination_t dst = (msg_destination_t)temp_header.destination_id;
                    // Сохраняем для всех destination (WIN, ESP32, STM32)
                    if (dst == MSG_DST_WIN || dst == MSG_DST_ESP32 || dst == MSG_DST_STM32)
                    {
                        std::lock_guard<std::mutex> lock(g_destination_to_test_id_mutex);
                        g_destination_to_test_id[dst] = test_id;
                        std::cout << ANSI_INFO << "[udp] Сохранено соответствие: destination=" 
                                  << (dst == MSG_DST_WIN ? "WIN" : 
                                      dst == MSG_DST_ESP32 ? "ESP32" : "STM32") 
                                  << " -> test_id=" << test_id << ANSI_ENDL << std::endl;
                    }
                }
            }
            
            g_message_router->route_from_buffer(binary_data.data(), static_cast<u32>(binary_data.size()));
        }
        else
        {
            std::cerr << "[udp] Failed to convert JSON to binary format" << std::endl;
        }
    }
}

int main()
{
    g_tcp_ptr = &tcp;  // Устанавливаем глобальный указатель на TCP
    g_wifi_ptr = &wifi;  // Устанавливаем глобальный указатель на WiFi
    g_message_router = &message_router;  // Устанавливаем глобальный указатель на message_router
    g_udp_api = &udp_api;  // Устанавливаем глобальный указатель на UDP API

    // Настраиваем message_router
    message_router.set_tcp_transport(&tcp);
    message_router.set_udp_transport(&udp_api);
    
    // Регистрируем обработчик для всех сообщений
    message_router.register_handler(MSG_TYPE_COMMAND, handle_new_message);
    message_router.register_handler(MSG_TYPE_DATA, handle_new_message);
    message_router.register_handler(MSG_TYPE_STREAM, handle_new_message);
    message_router.register_handler(MSG_TYPE_RESPONSE, handle_new_message);
    message_router.register_handler(MSG_TYPE_ERROR, handle_new_message);

    // Устанавливаем обработчик принятых пакетов (для обратной совместимости)
    tcp.set_packet_callback(handle_packet);
    
    // Настраиваем UDP API
    udp_api.set_json_callback(handle_udp_json);

    std::thread t_console(console_thread);

    // Запускаем WiFi
    if (!wifi.start())
    {
        std::cout << ANSI_ERR << "[wifi] WiFi start failed" << ANSI_ENDL << std::endl;
        g_running.store(false);
        t_console.join();
        return 1;
    }

    // Запускаем TCP
    if (!tcp.start())
    {
        std::cout << ANSI_ERR << "[tcp] TCP start failed" << ANSI_ENDL << std::endl;
        g_running.store(false);
        wifi.stop();
        t_console.join();
        return 1;
    }

    // Запускаем UDP API
    if (!udp_api.start())
    {
        std::cout << ANSI_WARN << "[udp] UDP API start failed (non-critical)" << ANSI_ENDL << std::endl;
        // Не критично, продолжаем работу
    }

    std::cout << ANSI_SUCC << "[main] WiFi, TCP and UDP servers started. Waiting for connections..." << ANSI_ENDL << std::endl;

    // Основной цикл - просто ждем завершения
    while (g_running.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    udp_api.stop();
    wifi.stop();
    tcp.stop();
    t_console.join();

    return 0;
}
