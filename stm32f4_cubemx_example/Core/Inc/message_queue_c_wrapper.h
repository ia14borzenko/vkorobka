#ifndef MESSAGE_QUEUE_C_WRAPPER_H
#define MESSAGE_QUEUE_C_WRAPPER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "message_protocol.h"

// Непрозрачный указатель на C++ объект
typedef void* message_queue_handle_t;

// C-интерфейс для работы с message_queue_t

// Создать экземпляр message_queue
message_queue_handle_t message_queue_create(void);

// Уничтожить экземпляр message_queue
void message_queue_destroy(message_queue_handle_t handle);

// Добавить сообщение в очередь
void message_queue_enqueue(message_queue_handle_t handle,
                           const msg_header_t* header,
                           const u8* payload,
                           u32 payload_len);

// Получить следующее сообщение из очереди (по приоритету)
// payload_out: буфер для payload (может быть NULL, если не нужен)
// max_payload_size: максимальный размер буфера payload_out
// payload_len_out: указатель для сохранения реальной длины payload (может быть NULL)
// Возвращает: 1 при успехе, 0 если очередь пуста
int message_queue_dequeue(message_queue_handle_t handle,
                          msg_header_t* header_out,
                          u8* payload_out,
                          u32 max_payload_size,
                          u32* payload_len_out);

// Проверить, пуста ли очередь
// Возвращает: 1 если пуста, 0 если есть сообщения
int message_queue_empty(message_queue_handle_t handle);

// Получить размер очереди
u32 message_queue_size(message_queue_handle_t handle);

// Очистить очередь
void message_queue_clear(message_queue_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif // MESSAGE_QUEUE_C_WRAPPER_H
