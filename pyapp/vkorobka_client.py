"""
Клиент для работы с vkorobka приложением через UDP
"""
import socket
import json
import threading
import time
import uuid
from typing import Optional, Dict, Callable
from image_utils import image_to_base64, base64_to_image


class VkorobkaClient:
    """Клиент для коммуникации с win-x64 приложением через UDP"""
    
    def __init__(self, server_host='127.0.0.1', server_port=1236, timeout=10.0):
        """
        Инициализация клиента
        
        Args:
            server_host: IP адрес win-x64 приложения
            server_port: UDP порт (по умолчанию 1236)
            timeout: Таймаут ожидания ответа в секундах
        """
        self.server_host = server_host
        self.server_port = server_port
        self.timeout = timeout
        
        # Создаем UDP сокет
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(('', 0))  # Случайный порт для получения ответов
        self.sock.settimeout(timeout)
        
        # Словарь для хранения ожидаемых ответов (test_id -> response_data)
        self.pending_responses: Dict[str, Dict] = {}
        self.response_lock = threading.Lock()
        
        # Флаг для остановки потока приема
        self.running = True
        
        # Запускаем поток для приема ответов
        self.receive_thread = threading.Thread(target=self._receive_loop, daemon=True)
        self.receive_thread.start()
        
        print(f"[client] Инициализирован клиент для {server_host}:{server_port}")
    
    def _receive_loop(self):
        """Цикл приема сообщений в отдельном потоке"""
        while self.running:
            try:
                data, addr = self.sock.recvfrom(65507)  # Максимальный размер UDP пакета
                
                # Логирование полученных данных
                print(f"[client] [RX] Получено {len(data)} байт от {addr[0]}:{addr[1]}")
                if len(data) > 0:
                    # Показываем первые 200 символов для отладки
                    preview = data[:200].decode('utf-8', errors='ignore')
                    print(f"[client] [RX] Превью данных (первые 200 символов): {preview}")
                
                try:
                    message = json.loads(data.decode('utf-8'))
                    print(f"[client] [RX] JSON успешно распарсен:")
                    print(f"[client] [RX]   type: {message.get('type', 'N/A')}")
                    print(f"[client] [RX]   source: {message.get('source', 'N/A')}")
                    print(f"[client] [RX]   destination: {message.get('destination', 'N/A')}")
                    test_id_received = message.get('test_id', 'N/A')
                    print(f"[client] [RX]   test_id: '{test_id_received}' (длина: {len(str(test_id_received))})")
                    print(f"[client] [RX]   payload_len: {len(message.get('payload', ''))} символов (base64)")
                    
                    # Отладочный вывод: показываем все ключи и значения
                    print(f"[client] [RX]   Все ключи в сообщении: {list(message.keys())}")
                    if 'test_id' in message:
                        print(f"[client] [RX]   test_id значение (repr): {repr(message['test_id'])}")
                    
                    self._handle_response(message)
                except json.JSONDecodeError as e:
                    print(f"[client] [ERROR] Ошибка парсинга JSON: {e}")
                    print(f"[client] [ERROR] Полные данные (первые 500 байт): {data[:500].decode('utf-8', errors='ignore')}")
                    print(f"[client] [DISCARD] Сообщение отброшено из-за невалидного JSON")
                except Exception as e:
                    print(f"[client] [ERROR] Ошибка обработки ответа: {e}")
                    import traceback
                    traceback.print_exc()
                    print(f"[client] [DISCARD] Сообщение отброшено из-за ошибки обработки")
                    
            except socket.timeout:
                # Таймаут - это нормально, просто продолжаем
                continue
            except Exception as e:
                if self.running:
                    print(f"[client] [ERROR] Критическая ошибка приема данных: {e}")
                    import traceback
                    traceback.print_exc()
                break
    
    def _handle_response(self, message: Dict):
        """Обработка полученного ответа"""
        # Проверяем наличие test_id для сопоставления с запросом
        test_id = message.get('test_id')
        
        print(f"[client] [HANDLE] Обработка ответа с test_id={test_id}")
        
        if not test_id:
            print(f"[client] [DISCARD] Сообщение отброшено: отсутствует test_id")
            print(f"[client] [DISCARD] Доступные ключи в сообщении: {list(message.keys())}")
            return
        
        if test_id not in self.pending_responses:
            print(f"[client] [DISCARD] Сообщение отброшено: test_id={test_id} не найден в ожидающих ответах")
            print(f"[client] [DISCARD] Ожидаемые test_id: {list(self.pending_responses.keys())}")
            return
        
        print(f"[client] [ACCEPT] Сообщение принято для test_id={test_id}")
        with self.response_lock:
            self.pending_responses[test_id] = message
            # Уведомляем ожидающий поток
            self.pending_responses[test_id]['_received'] = True
            print(f"[client] [ACCEPT] Ответ зарегистрирован и готов к извлечению")
    
    def send_test_message(self, destination: str, image_data: bytes, test_id: Optional[str] = None) -> str:
        """
        Отправляет тестовое сообщение с изображением
        
        Args:
            destination: Назначение ("win", "esp32", "stm32")
            image_data: JPG данные изображения
            test_id: Уникальный ID теста (генерируется автоматически если не указан)
        
        Returns:
            str: test_id для отслеживания ответа
        """
        if test_id is None:
            test_id = str(uuid.uuid4())
        
        # Конвертируем изображение в base64
        image_b64 = image_to_base64(image_data)
        
        # Формируем JSON сообщение
        message = {
            "type": "data",
            "source": "external",
            "destination": destination,
            "priority": 128,
            "stream_id": 0,
            "payload": image_b64,
            "seq": 0,
            "test_id": test_id
        }
        
        # Регистрируем ожидание ответа
        with self.response_lock:
            self.pending_responses[test_id] = {'_received': False}
        
        # Отправляем сообщение
        json_str = json.dumps(message)
        self.sock.sendto(json_str.encode('utf-8'), (self.server_host, self.server_port))
        
        print(f"[client] Отправлено сообщение test_id={test_id} для destination={destination}, размер={len(image_data)} байт")
        
        return test_id
    
    def wait_for_response(self, test_id: str, timeout: Optional[float] = None) -> Optional[Dict]:
        """
        Ожидает ответ на тестовое сообщение
        
        Args:
            test_id: ID теста
            timeout: Таймаут ожидания (использует self.timeout если не указан)
        
        Returns:
            Dict с ответом или None при таймауте
        """
        if timeout is None:
            timeout = self.timeout
        
        start_time = time.time()
        check_count = 0
        
        print(f"[client] [WAIT] Ожидание ответа для test_id={test_id}, таймаут={timeout} сек")
        
        while time.time() - start_time < timeout:
            elapsed = time.time() - start_time
            check_count += 1
            
            # Логируем каждые 10 проверок (примерно раз в секунду)
            if check_count % 10 == 0:
                print(f"[client] [WAIT] Все еще ждем... прошло {elapsed:.1f} сек из {timeout} сек")
            
            with self.response_lock:
                if test_id in self.pending_responses:
                    response = self.pending_responses[test_id]
                    if response.get('_received', False):
                        # Удаляем из ожидающих и возвращаем
                        del self.pending_responses[test_id]
                        response.pop('_received', None)
                        print(f"[client] [WAIT] Ответ получен через {elapsed:.2f} сек")
                        return response
            
            time.sleep(0.1)  # Небольшая задержка
        
        # Таймаут
        elapsed = time.time() - start_time
        print(f"[client] [TIMEOUT] Таймаут ожидания ответа для test_id={test_id} (прошло {elapsed:.2f} сек)")
        print(f"[client] [TIMEOUT] Проверено {check_count} раз")
        
        with self.response_lock:
            if test_id in self.pending_responses:
                print(f"[client] [TIMEOUT] Удаляем test_id={test_id} из ожидающих")
                del self.pending_responses[test_id]
        
        return None
    
    def test_component(self, destination: str, image_data: bytes) -> Optional[bytes]:
        """
        Тестирует компонент: отправляет изображение и получает отзеркаленное
        
        Args:
            destination: Назначение ("win", "esp32", "stm32")
            image_data: JPG данные для отправки
        
        Returns:
            bytes: Отзеркаленное изображение или None при ошибке
        """
        print(f"\n[test] Тестирование компонента: {destination}")
        print(f"[test] Отправка изображения размером {len(image_data)} байт ({len(image_data)/1024:.2f} КБ)")
        
        # Отправляем сообщение
        test_id = self.send_test_message(destination, image_data)
        print(f"[test] [TX] Сообщение отправлено, test_id={test_id}")
        
        # Ждем ответ
        print(f"[test] [WAIT] Ожидание ответа (таймаут {self.timeout} сек)...")
        response = self.wait_for_response(test_id, timeout=self.timeout)
        
        if response is None:
            print(f"[test] [FAIL] Таймаут ожидания ответа от {destination}")
            print(f"[test] [FAIL] Проверьте логи выше для деталей о полученных/отброшенных сообщениях")
            return None
        
        print(f"[test] [RX] Ответ получен, проверяем содержимое...")
        
        # Проверяем тип ответа
        response_type = response.get('type')
        print(f"[test] [CHECK] Тип ответа: {response_type}")
        if response_type != 'response':
            print(f"[test] [FAIL] Неожиданный тип ответа: {response_type}, ожидался 'response'")
            print(f"[test] [DISCARD] Полный ответ: {response}")
            return None
        
        # Проверяем source
        response_source = response.get('source')
        print(f"[test] [CHECK] Source ответа: {response_source}, ожидался: {destination}")
        if response_source != destination:
            print(f"[test] [WARN] Неожиданный source в ответе: {response_source}, ожидался {destination}")
            print(f"[test] [WARN] Продолжаем обработку, но это может быть ошибка")
        
        # Извлекаем payload
        payload_b64 = response.get('payload', '')
        print(f"[test] [CHECK] Payload присутствует: {len(payload_b64) > 0}, длина base64: {len(payload_b64)} символов")
        if not payload_b64:
            print(f"[test] [FAIL] Пустой payload в ответе")
            print(f"[test] [DISCARD] Полный ответ: {response}")
            return None
        
        try:
            # Декодируем base64
            print(f"[test] [DECODE] Декодирование base64 payload...")
            received_image = base64_to_image(payload_b64)
            print(f"[test] [SUCCESS] Получен ответ размером {len(received_image)} байт ({len(received_image)/1024:.2f} КБ)")
            print(f"[test] [SUCCESS] Исходный размер: {len(image_data)} байт, полученный: {len(received_image)} байт")
            return received_image
        except Exception as e:
            print(f"[test] [FAIL] Ошибка декодирования ответа: {e}")
            import traceback
            traceback.print_exc()
            print(f"[test] [DISCARD] Payload (первые 200 символов): {payload_b64[:200]}")
            return None
    
    def close(self):
        """Закрывает соединение"""
        self.running = False
        if self.sock:
            self.sock.close()
        print("[client] Клиент закрыт")
