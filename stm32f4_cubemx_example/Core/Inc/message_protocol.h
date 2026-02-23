#ifndef MESSAGE_PROTOCOL_H
#define MESSAGE_PROTOCOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "my_types.h"

// Размер заголовка сообщения (12 bytes)
#define MSG_HEADER_LEN 12

// Типы сообщений
typedef enum
{
    MSG_TYPE_COMMAND = 0x01,  // Разовые команды
    MSG_TYPE_DATA = 0x02,     // Полезные данные
    MSG_TYPE_STREAM = 0x03,   // Потоковые данные
    MSG_TYPE_RESPONSE = 0x04, // Ответы на команды
    MSG_TYPE_ERROR = 0x05     // Сообщения об ошибках
} msg_type_t;

// ID компонентов системы
typedef enum
{
    MSG_SRC_WIN = 0x01,       // Windows приложение
    MSG_SRC_ESP32 = 0x02,     // ESP32 микроконтроллер
    MSG_SRC_STM32 = 0x03,     // STM32 микроконтроллер
    MSG_SRC_EXTERNAL = 0x04   // Внешнее приложение (Python и т.д.)
} msg_source_t;

// ID получателей
typedef enum
{
    MSG_DST_WIN = 0x01,       // Windows приложение
    MSG_DST_ESP32 = 0x02,     // ESP32 микроконтроллер
    MSG_DST_STM32 = 0x03,     // STM32 микроконтроллер
    MSG_DST_EXTERNAL = 0x04,  // Внешнее приложение
    MSG_DST_BROADCAST = 0xFF  // Широковещательная рассылка
} msg_destination_t;

// Флаги маршрутизации (битовые флаги)
#define MSG_ROUTE_NONE             0x00  // Без специальных флагов
#define MSG_ROUTE_FLAG_REQUIRE_ACK 0x01  // Требуется подтверждение
#define MSG_ROUTE_FLAG_RELIABLE    0x02  // Надежная доставка
#define MSG_ROUTE_FLAG_COMPRESSED  0x04  // Данные сжаты
#define MSG_ROUTE_FLAG_ENCRYPTED   0x08  // Данные зашифрованы

// Максимальный размер payload
#define MSG_MAX_PAYLOAD_SIZE 65535

// Структура заголовка сообщения (12 bytes total)
// Layout: [0:msg_type][1:source][2:dest][3:flags][4:priority][5-6:stream_id][7-10:payload_len][11:sequence]
typedef struct
{
    u8 msg_type;          // [0] Тип сообщения (msg_type_t)
    u8 source_id;         // [1] ID источника (msg_source_t)
    u8 destination_id;    // [2] ID получателя (msg_destination_t)
    u8 route_flags;       // [3] Флаги маршрутизации
    u8 priority;          // [4] Приоритет (0 = highest, 255 = lowest)
    u16 stream_id;        // [5-6] ID потока для потоковых данных (little-endian)
    u32 payload_len;      // [7-10] Длина payload (little-endian)
    u8 sequence;          // [11] Порядковый номер для упорядочивания (0-255)
} msg_header_t;

// Структура полного сообщения (для удобства работы)
typedef struct
{
    msg_header_t header;
    const u8* payload;    // Указатель на payload (не владеет памятью)
} message_t;

/**
 * Упаковать сообщение в бинарный формат
 * \param header Заголовок сообщения
 * \param payload Данные payload (может быть NULL)
 * \param payload_len Длина payload
 * \param out_buffer Выходной буфер (должен быть размером не менее MSG_HEADER_LEN + payload_len)
 * \return Общий размер сообщения (MSG_HEADER_LEN + payload_len) или 0 при ошибке
 */
u32 msg_pack(const msg_header_t* header, const void* payload, u32 payload_len, u8* out_buffer);

/**
 * Распарсить сообщение из бинарного формата
 * \param rx_msg Входящий буфер с данными
 * \param rx_msg_len Длина входящего буфера
 * \param out_header Указатель для сохранения заголовка
 * \param out_payload Указатель для сохранения указателя на payload
 * \param out_payload_len Указатель для сохранения длины payload
 * \return 1 если сообщение успешно распарсено, 0 если данных недостаточно или ошибка
 */
int msg_unpack(const u8* rx_msg, u32 rx_msg_len, msg_header_t* out_header, 
               const u8** out_payload, u32* out_payload_len);

/**
 * Проверить валидность заголовка сообщения
 * \param header Заголовок для проверки
 * \return 1 если заголовок валиден, 0 если нет
 */
int msg_validate_header(const msg_header_t* header);

/**
 * Создать заголовок сообщения
 * \param msg_type Тип сообщения
 * \param source_id ID источника
 * \param destination_id ID получателя
 * \param priority Приоритет
 * \param stream_id ID потока (0 для не-потоковых данных)
 * \param payload_len Длина payload
 * \param sequence Порядковый номер
 * \param route_flags Флаги маршрутизации
 * \return Заголовок сообщения
 */
msg_header_t msg_create_header(msg_type_t msg_type, msg_source_t source_id, 
                               msg_destination_t destination_id, u8 priority,
                               u16 stream_id, u32 payload_len, u8 sequence, u8 route_flags);

#ifdef __cplusplus
}
#endif

#endif // MESSAGE_PROTOCOL_H
