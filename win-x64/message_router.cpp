#include "message_router.hpp"
#include "tcp.hpp"
#include "udp_api.hpp"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <vector>

// Простая реализация base64 декодирования
static std::vector<u8> base64_decode(const std::string& encoded)
{
    const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<u8> result;
    int val = 0, valb = -8;
    
    for (unsigned char c : encoded)
    {
        if (c == '=') break;
        if (chars.find(c) == std::string::npos) continue;
        
        val = (val << 6) + static_cast<int>(chars.find(c));
        valb += 6;
        
        if (valb >= 0)
        {
            result.push_back((val >> valb) & 0xFF);
            valb -= 8;
        }
    }
    
    return result;
}

// Простая реализация base64 кодирования
static std::string base64_encode(const u8* data, size_t len)
{
    const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    int val = 0, valb = -6;
    
    for (size_t i = 0; i < len; ++i)
    {
        val = (val << 8) + data[i];
        valb += 8;
        
        while (valb >= 0)
        {
            result += chars[(val >> valb) & 0x3F];
            valb -= 6;
        }
    }
    
    if (valb > -6)
    {
        result += chars[((val << 8) >> (valb + 8)) & 0x3F];
    }
    
    while (result.size() % 4)
    {
        result += '=';
    }
    
    return result;
}

// Простой JSON парсер (для базовой функциональности)
// В продакшене лучше использовать библиотеку типа nlohmann/json

// Вспомогательная функция для извлечения значений из JSON
static std::string extract_json_value(const std::string& json, const std::string& key)
{
    std::string search_key = "\"" + key + "\"";
    size_t key_pos = json.find(search_key);
    if (key_pos == std::string::npos)
    {
        return "";
    }
    
    size_t colon_pos = json.find(':', key_pos);
    if (colon_pos == std::string::npos)
    {
        return "";
    }
    
    size_t start = colon_pos + 1;
    // Пропускаем пробелы
    while (start < json.length() && (json[start] == ' ' || json[start] == '\t'))
    {
        start++;
    }
    
    if (start >= json.length())
    {
        return "";
    }
    
    // Если значение в кавычках
    if (json[start] == '"')
    {
        start++;
        size_t end = json.find('"', start);
        if (end == std::string::npos)
        {
            return "";
        }
        return json.substr(start, end - start);
    }
    else
    {
        // Числовое значение
        size_t end = start;
        while (end < json.length() && json[end] != ',' && json[end] != '}' && json[end] != ' ')
        {
            end++;
        }
        return json.substr(start, end - start);
    }
}

message_router_t::message_router_t()
    : tcp_transport_(nullptr)
    , udp_transport_(nullptr)
    , sequence_counter_(0)
{
}

message_router_t::~message_router_t()
{
}

void message_router_t::register_handler(msg_type_t msg_type, message_handler_t handler)
{
    std::lock_guard<std::mutex> lock(mutex_);
    type_handlers_[msg_type].push_back(handler);
}

void message_router_t::register_handler(msg_destination_t destination, message_handler_t handler)
{
    std::lock_guard<std::mutex> lock(mutex_);
    destination_handlers_[destination].push_back(handler);
}

void message_router_t::register_handler(msg_type_t msg_type, msg_destination_t destination, message_handler_t handler)
{
    std::lock_guard<std::mutex> lock(mutex_);
    combined_handlers_[std::make_pair(msg_type, destination)].push_back(handler);
}

bool message_router_t::route_message(const msg_header_t& header, const u8* payload, u32 payload_len)
{
    // Проверка валидности заголовка
    if (!msg_validate_header(&header))
    {
        std::cerr << "[router] Invalid message header" << std::endl;
        return false;
    }

    // Проверка destination и маршрутизация
    msg_destination_t dst = (msg_destination_t)header.destination_id;
    
    // Если сообщение для нас (WIN) или BROADCAST, обрабатываем локально
    if (dst == MSG_DST_WIN || dst == MSG_DST_BROADCAST)
    {
        invoke_handlers(header, payload, payload_len);
    }
    
    // Если сообщение для EXTERNAL (Python), тоже обрабатываем локально (чтобы переслать через UDP)
    if (dst == MSG_DST_EXTERNAL)
    {
        invoke_handlers(header, payload, payload_len);
    }
    
    // Если сообщение для другого компонента (ESP32/STM32), отправляем через транспорт
    if (dst == MSG_DST_ESP32 || dst == MSG_DST_STM32)
    {
        return send_message(header, payload, payload_len);
    }
    
    return true;
}

bool message_router_t::route_from_buffer(const u8* buffer, u32 buffer_len)
{
    if (buffer == nullptr || buffer_len < MSG_HEADER_LEN)
    {
        return false;
    }

    msg_header_t header;
    const u8* payload = nullptr;
    u32 payload_len = 0;

    if (!msg_unpack(buffer, buffer_len, &header, &payload, &payload_len))
    {
        return false;
    }
    
    return route_message(header, payload, payload_len);
}

bool message_router_t::convert_json_to_binary(const std::string& json_str, std::vector<u8>& out_binary)
{
    msg_header_t header;
    std::vector<u8> payload;
    std::string test_id;
    
    if (!parse_json_message(json_str, header, payload, test_id))
    {
        return false;
    }
    
    // Сохраняем test_id для последующего использования (упрощенная версия)
    // В реальной реализации test_id должен передаваться через контекст сообщения
    
    // Упаковываем в бинарный формат
    out_binary.resize(MSG_HEADER_LEN + payload.size());
    u32 packed_size = msg_pack(&header, payload.data(), static_cast<u32>(payload.size()), out_binary.data());
    
    if (packed_size == 0)
    {
        return false;
    }
    
    out_binary.resize(packed_size);
    return true;
}

bool message_router_t::convert_binary_to_json(const msg_header_t& header, const u8* payload, u32 payload_len, std::string& out_json, const std::string& test_id)
{
    out_json = create_json_message(header, payload, payload_len, test_id);
    return !out_json.empty();
}

void message_router_t::set_tcp_transport(tcp_t* tcp)
{
    std::lock_guard<std::mutex> lock(mutex_);
    tcp_transport_ = tcp;
}

void message_router_t::set_udp_transport(udp_api_t* udp)
{
    std::lock_guard<std::mutex> lock(mutex_);
    udp_transport_ = udp;
}

bool message_router_t::send_message(const msg_header_t& header, const u8* payload, u32 payload_len)
{
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::cout << "[router] [DEBUG] send_message called: dst=" << static_cast<int>(header.destination_id) 
              << ", payload_len=" << payload_len << ", header.payload_len=" << header.payload_len << std::endl;
    
    // Упаковываем сообщение
    std::vector<u8> buffer(MSG_HEADER_LEN + payload_len);
    u32 packed_size = msg_pack(&header, payload, payload_len, buffer.data());
    
    std::cout << "[router] [DEBUG] msg_pack result: packed_size=" << packed_size << std::endl;
    
    if (packed_size == 0)
    {
        std::cerr << "[router] [ERROR] msg_pack failed!" << std::endl;
        return false;
    }
    
    msg_destination_t dst = (msg_destination_t)header.destination_id;
    
    // Определяем транспорт в зависимости от получателя
    if (dst == MSG_DST_ESP32 || dst == MSG_DST_STM32)
    {
        std::cout << "[router] [DEBUG] Sending to " << (dst == MSG_DST_ESP32 ? "ESP32" : "STM32") 
                  << " via TCP, total size=" << packed_size << " bytes" << std::endl;
        // Отправляем через TCP (к ESP32)
        if (tcp_transport_ && tcp_transport_->is_connected())
        {
            // Используем существующий метод send для отправки бинарных данных
            bool result = tcp_transport_->send(reinterpret_cast<const char*>(buffer.data()), static_cast<int>(packed_size));
            std::cout << "[router] [DEBUG] TCP send result: " << (result ? "SUCCESS" : "FAILED") << std::endl;
            return result;
        }
        else
        {
            std::cerr << "[router] TCP not connected, cannot send message" << std::endl;
            return false;
        }
    }
    else if (dst == MSG_DST_EXTERNAL)
    {
        // Отправляем через UDP (к внешним приложениям)
        if (udp_transport_)
        {
            // Конвертируем в JSON и отправляем
            std::string json_str;
            if (convert_binary_to_json(header, payload, payload_len, json_str))
            {
                return udp_transport_->send_json_to_all(json_str);
            }
        }
    }
    
    return false;
}

u8 message_router_t::get_next_sequence()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return ++sequence_counter_;
}

void message_router_t::invoke_handlers(const msg_header_t& header, const u8* payload, u32 payload_len)
{
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Вызываем комбинированные обработчики
    auto combined_key = std::make_pair((msg_type_t)header.msg_type, (msg_destination_t)header.destination_id);
    auto it_combined = combined_handlers_.find(combined_key);
    if (it_combined != combined_handlers_.end())
    {
        for (auto& handler : it_combined->second)
        {
            handler(header, payload, payload_len);
        }
    }
    
    // Вызываем обработчики по типу
    auto it_type = type_handlers_.find((msg_type_t)header.msg_type);
    if (it_type != type_handlers_.end())
    {
        for (auto& handler : it_type->second)
        {
            handler(header, payload, payload_len);
        }
    }
    
    // Вызываем обработчики по получателю
    auto it_dest = destination_handlers_.find((msg_destination_t)header.destination_id);
    if (it_dest != destination_handlers_.end())
    {
        for (auto& handler : it_dest->second)
        {
            handler(header, payload, payload_len);
        }
    }
}

bool message_router_t::parse_json_message(const std::string& json_str, msg_header_t& header, std::vector<u8>& payload, std::string& test_id)
{
    // Простой парсер JSON (базовая реализация)
    // В продакшене использовать библиотеку типа nlohmann/json
    
    // Ищем поля в JSON строке
    size_t type_pos = json_str.find("\"type\"");
    size_t source_pos = json_str.find("\"source\"");
    size_t destination_pos = json_str.find("\"destination\"");
    size_t priority_pos = json_str.find("\"priority\"");
    size_t stream_id_pos = json_str.find("\"stream_id\"");
    size_t payload_pos = json_str.find("\"payload\"");
    size_t seq_pos = json_str.find("\"seq\"");
    
    if (type_pos == std::string::npos || destination_pos == std::string::npos)
    {
        return false;
    }
    
    // Парсим тип
    std::string type_str = extract_json_value(json_str, "type");
    if (type_str == "command") header.msg_type = MSG_TYPE_COMMAND;
    else if (type_str == "data") header.msg_type = MSG_TYPE_DATA;
    else if (type_str == "stream") header.msg_type = MSG_TYPE_STREAM;
    else if (type_str == "response") header.msg_type = MSG_TYPE_RESPONSE;
    else if (type_str == "error") header.msg_type = MSG_TYPE_ERROR;
    else return false;
    
    // Парсим source
    std::string source_str = extract_json_value(json_str, "source");
    header.source_id = MSG_SRC_EXTERNAL; // По умолчанию external
    
    // Парсим destination
    std::string dest_str = extract_json_value(json_str, "destination");
    if (dest_str == "win") header.destination_id = MSG_DST_WIN;
    else if (dest_str == "esp32") header.destination_id = MSG_DST_ESP32;
    else if (dest_str == "stm32") header.destination_id = MSG_DST_STM32;
    else if (dest_str == "external") header.destination_id = MSG_DST_EXTERNAL;
    else if (dest_str == "broadcast") header.destination_id = MSG_DST_BROADCAST;
    else return false;
    
    // Парсим priority
    if (priority_pos != std::string::npos)
    {
        std::string priority_str = extract_json_value(json_str, "priority");
        header.priority = static_cast<u8>(std::stoi(priority_str));
    }
    else
    {
        header.priority = 128; // Средний приоритет по умолчанию
    }
    
    // Парсим stream_id
    if (stream_id_pos != std::string::npos)
    {
        std::string stream_str = extract_json_value(json_str, "stream_id");
        header.stream_id = static_cast<u16>(std::stoi(stream_str));
    }
    else
    {
        header.stream_id = 0;
    }
    
    // Парсим sequence
    if (seq_pos != std::string::npos)
    {
        std::string seq_str = extract_json_value(json_str, "seq");
        header.sequence = static_cast<u8>(std::stoi(seq_str));
    }
    else
    {
        header.sequence = get_next_sequence();
    }
    
    // Парсим payload (base64 декодируем)
    if (payload_pos != std::string::npos)
    {
        std::string payload_str = extract_json_value(json_str, "payload");
        std::cout << "[router] [DEBUG] Extracted payload base64 string length: " << payload_str.length() << std::endl;
        if (payload_str.length() > 0 && payload_str.length() <= 100)
        {
            std::cout << "[router] [DEBUG] Payload base64 (first 100 chars): " << payload_str << std::endl;
        }
        // Декодируем base64
        payload = base64_decode(payload_str);
        std::cout << "[router] [DEBUG] Decoded payload size: " << payload.size() << " bytes" << std::endl;
        if (payload.size() > 0 && payload.size() <= 16)
        {
            std::cout << "[router] [DEBUG] Payload data (hex): ";
            for (size_t i = 0; i < payload.size(); ++i)
            {
                std::cout << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(payload[i]) << " ";
            }
            std::cout << std::dec << std::endl;
        }
    }
    else
    {
        payload.clear();
        std::cout << "[router] [DEBUG] No payload field found in JSON" << std::endl;
    }
    
    // Парсим test_id если есть
    size_t test_id_pos = json_str.find("\"test_id\"");
    test_id.clear();
    if (test_id_pos != std::string::npos)
    {
        test_id = extract_json_value(json_str, "test_id");
    }
    
    header.payload_len = static_cast<u32>(payload.size());
    std::cout << "[router] [DEBUG] Final header.payload_len: " << header.payload_len << std::endl;
    header.route_flags = 0;
    
    return true;
}

std::string message_router_t::create_json_message(const msg_header_t& header, const u8* payload, u32 payload_len, const std::string& test_id)
{
    std::ostringstream json;
    json << "{";
    
    // type
    json << "\"type\":\"";
    switch (header.msg_type)
    {
    case MSG_TYPE_COMMAND: json << "command"; break;
    case MSG_TYPE_DATA: json << "data"; break;
    case MSG_TYPE_STREAM: json << "stream"; break;
    case MSG_TYPE_RESPONSE: json << "response"; break;
    case MSG_TYPE_ERROR: json << "error"; break;
    default: json << "unknown"; break;
    }
    json << "\",";
    
    // source
    json << "\"source\":\"";
    switch (header.source_id)
    {
    case MSG_SRC_WIN: json << "win"; break;
    case MSG_SRC_ESP32: json << "esp32"; break;
    case MSG_SRC_STM32: json << "stm32"; break;
    case MSG_SRC_EXTERNAL: json << "external"; break;
    default: json << "unknown"; break;
    }
    json << "\",";
    
    // destination
    json << "\"destination\":\"";
    switch (header.destination_id)
    {
    case MSG_DST_WIN: json << "win"; break;
    case MSG_DST_ESP32: json << "esp32"; break;
    case MSG_DST_STM32: json << "stm32"; break;
    case MSG_DST_EXTERNAL: json << "external"; break;
    case MSG_DST_BROADCAST: json << "broadcast"; break;
    default: json << "unknown"; break;
    }
    json << "\",";
    
    // priority
    json << "\"priority\":" << static_cast<int>(header.priority) << ",";
    
    // stream_id
    json << "\"stream_id\":" << header.stream_id << ",";
    
    // seq
    json << "\"seq\":" << static_cast<int>(header.sequence) << ",";
    
    // payload (base64 кодируем)
    json << "\"payload\":\"";
    if (payload && payload_len > 0)
    {
        std::string base64_payload = base64_encode(payload, payload_len);
        json << base64_payload;
    }
    json << "\"";
    
    // test_id если указан
    if (!test_id.empty())
    {
        json << ",\"test_id\":\"" << test_id << "\"";
    }
    
    json << "}";
    return json.str();
}
