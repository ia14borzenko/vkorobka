"""
Клиент для работы с vkorobka приложением через UDP
"""
import socket
import json
import sys
import threading
import time
import uuid
from typing import Optional, Dict, Callable, Any
from image_utils import image_to_base64, base64_to_image


class VkorobkaClient:
    """Клиент для коммуникации с win-x64 приложением через UDP"""
    
    def __init__(
        self,
        server_host='127.0.0.1',
        server_port=1236,
        timeout=10.0,
        verbose_udp: bool = True,
    ):
        """
        Инициализация клиента
        
        Args:
            server_host: IP адрес win-x64 приложения
            server_port: UDP порт (по умолчанию 1236)
            timeout: Таймаут ожидания ответа в секундах
            verbose_udp: Подробный лог входящих UDP (для потоков лучше False)
        """
        self.server_host = server_host
        self.server_port = server_port
        self.timeout = timeout
        self.verbose_udp = verbose_udp
        # Обработчики MSG_TYPE_STREAM с ESP32 (stream_id -> callback)
        self._stream_handlers: Dict[int, Callable[[Dict[str, Any]], None]] = {}
        
        # Создаем UDP сокет
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(('', 0))  # Случайный порт для получения ответов
        self.sock.settimeout(timeout)
        
        # Словарь для хранения ожидаемых ответов (test_id -> response_data)
        self.pending_responses: Dict[str, Dict] = {}
        self.response_lock = threading.Lock()
        
        # Словарь для ожидания подтверждений чанков (chunk_ack_key -> received)
        # chunk_ack_key = f"{test_id}:{chunk_index}" для текущего потока
        self.pending_chunk_acks: Dict[str, bool] = {}
        # Текущий test_id для потока (для сопоставления подтверждений)
        self.current_stream_test_id: Optional[str] = None
        
        # Флаг для остановки потока приема
        self.running = True
        
        # Запускаем поток для приема ответов
        self.receive_thread = threading.Thread(target=self._receive_loop, daemon=True)
        self.receive_thread.start()
        
        print(f"[client] Инициализирован клиент для {server_host}:{server_port}")

    def register_stream_handler(
        self, stream_id: int, handler: Callable[[Dict[str, Any]], None]
    ) -> None:
        """Вызывать для потоковых сообщений type=stream, source=esp32 (например PCM с микрофона)."""
        self._stream_handlers[stream_id] = handler
    
    def _receive_loop(self):
        """Цикл приема сообщений в отдельном потоке"""
        while self.running:
            try:
                data, addr = self.sock.recvfrom(65507)  # Максимальный размер UDP пакета
                
                if self.verbose_udp:
                    print(f"[client] [RX] Получено {len(data)} байт от {addr[0]}:{addr[1]}")
                    if len(data) > 0:
                        preview = data[:200].decode('utf-8', errors='ignore')
                        print(f"[client] [RX] Превью данных (первые 200 символов): {preview}")
                
                try:
                    message = json.loads(data.decode('utf-8'))
                    if self.verbose_udp:
                        print(f"[client] [RX] JSON успешно распарсен:")
                        print(f"[client] [RX]   type: {message.get('type', 'N/A')}")
                        print(f"[client] [RX]   source: {message.get('source', 'N/A')}")
                        print(f"[client] [RX]   destination: {message.get('destination', 'N/A')}")
                        test_id_received = message.get('test_id', 'N/A')
                        print(f"[client] [RX]   test_id: '{test_id_received}' (длина: {len(str(test_id_received))})")
                        print(f"[client] [RX]   payload_len: {len(message.get('payload', ''))} символов (base64)")
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
        """
        Обработка полученного ответа.
        
        ВАЖНО для ИИ-агента:
        - Payload в JSON всегда base64-кодирован (win-x64 кодирует все payload в base64)
        - CHUNK_ACK имеет формат "CHUNK_ACK:{chunk_index}" (например, "CHUNK_ACK:5")
        - CHUNK_ACK используется для flow control - Python ждёт подтверждения перед отправкой следующего чанка
        - Финальный ответ LCD_FRAME_OK приходит с тем же test_id, что и все чанки
        """
        if message.get('type') == 'stream' and message.get('source') == 'esp32':
            sid = int(message.get('stream_id', 0))
            if sid in self._stream_handlers:
                self._stream_handlers[sid](message)
                return

        # Проверяем, является ли это подтверждением чанка (CHUNK_ACK)
        # Payload в JSON всегда base64-кодирован, нужно декодировать
        payload_b64 = message.get('payload', '')
        if payload_b64:
            try:
                import base64
                payload_bytes = base64.b64decode(payload_b64)
                payload_decoded = payload_bytes.decode('utf-8', errors='ignore')
                if payload_decoded.startswith('CHUNK_ACK:'):
                    # Это подтверждение чанка
                    try:
                        chunk_index_str = payload_decoded.split(':')[1]
                        chunk_index = int(chunk_index_str)
                        seq = message.get('seq', -1)
                        
                        # Используем текущий test_id из потока или ищем по chunk_index
                        with self.response_lock:
                            found = False
                            # Сначала пробуем найти по текущему test_id
                            if self.current_stream_test_id:
                                chunk_ack_key = f"{self.current_stream_test_id}:{chunk_index}"
                                if chunk_ack_key in self.pending_chunk_acks:
                                    self.pending_chunk_acks[chunk_ack_key] = True
                                    print(f"[client] [CHUNK_ACK] Получено подтверждение для чанка {chunk_index} (test_id={self.current_stream_test_id}, seq={seq})")
                                    found = True
                            
                            # Если не нашли, ищем любой ключ с таким chunk_index
                            if not found:
                                for key in list(self.pending_chunk_acks.keys()):
                                    if key.endswith(f":{chunk_index}"):
                                        self.pending_chunk_acks[key] = True
                                        print(f"[client] [CHUNK_ACK] Получено подтверждение для чанка {chunk_index} (key={key}, seq={seq})")
                                        found = True
                                        break
                            
                            if not found:
                                print(f"[client] [CHUNK_ACK] Неожиданное подтверждение для чанка {chunk_index} (seq={seq})")
                    except (ValueError, IndexError) as e:
                        print(f"[client] [CHUNK_ACK] Ошибка парсинга CHUNK_ACK: {e}, payload={payload_decoded}")
                    return
            except Exception as e:
                # Если не удалось декодировать - это не CHUNK_ACK, продолжаем обычную обработку
                pass
        
        # Обычная обработка ответа по test_id
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
            destination: Назначение ("win", "esp32")
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

    # --- Новый API для потоковой передачи кадров на дисплей ---

    STREAM_ID_LCD_FRAME = 1
    STREAM_ID_DYN_PCM = 3

    @staticmethod
    def pack_pcm24_stream_payload_mono(
        mono_int24: Any, sample_rate_hz: int = 48000
    ) -> bytes:
        """
        Бинарный payload как у mic_stream (ESP32): u32 rate, u16 count, u16 bits=24,
        затем моно PCM 24-bit little-endian (несжатые отсчёты).
        mono_int24: последовательность int32 в диапазоне int24.
        """
        import numpy as np
        import struct

        arr = np.asarray(mono_int24, dtype=np.int32).ravel()
        n = int(arr.size)
        if n <= 0 or n > 512:
            raise ValueError(f"sample_count must be 1..512, got {n}")
        if sample_rate_hz != 48000:
            raise ValueError("ESP32 dyn_playback ожидает 48000 Hz")
        header = struct.pack("<IHH", int(sample_rate_hz), n, 24)
        out = bytearray(header)
        lo = -0x800000
        hi = 0x7FFFFF
        for i in range(n):
            v = int(arr[i])
            if v < lo:
                v = lo
            elif v > hi:
                v = hi
            out.extend(v.to_bytes(3, byteorder="little", signed=True))
        return bytes(out)

    def _send_stream_chunk(
        self,
        destination: str,
        stream_id: int,
        seq: int,
        chunk_payload_b64: str,
        test_id: Optional[str] = None,
        extra_fields: Optional[dict] = None,
        verbose: bool = True,
    ) -> None:
        """
        Внутренний метод отправки одного STREAM-сообщения с base64-чанком.
        test_id: если None или пустая строка — поле не добавляется (чтобы не затирать
        соответствие destination→test_id на win-x64 между dyn.on и dyn.off).
        """
        message = {
            "type": "stream",
            "source": "external",
            "destination": destination,
            "priority": 128,
            "stream_id": stream_id,
            "payload": chunk_payload_b64,
            "seq": seq,
        }
        if test_id:
            message["test_id"] = test_id

        if extra_fields:
            message.update(extra_fields)

        json_str = json.dumps(message)
        self.sock.sendto(json_str.encode("utf-8"), (self.server_host, self.server_port))

        if verbose:
            print(
                f"[client] [STREAM] Отправлен чанк seq={seq}, stream_id={stream_id}, "
                f"base64_len={len(chunk_payload_b64)}"
            )

    def send_lcd_frame_rgb565(
        self,
        destination: str,
        frame_bytes: bytes,
        width: int,
        height: int,
        format_code: int = 1,
        chunk_rows: int = 20,  # Количество строк в одном чанке (вместо размера в байтах)
    ) -> Optional[Dict]:
        """
        Отправляет RGB565 кадр на дисплей через потоковые сообщения (STREAM).
        Реализует потоковый вывод: чанки разбиваются по строкам и выводятся сразу на дисплей.

        Кадр разбивается на чанки по строкам, каждый чанк содержит внутренний заголовок:
        - u16 width
        - u16 height
        - u16 chunk_index
        - u16 total_chunks
        - u16 format_code
        - u16 y_start          (начальная Y-координата для этого чанка)
        - u16 height_chunk     (количество строк в этом чанке)
        - затем данные чанка (RGB565 big-endian, height_chunk строк)

        Args:
            destination: Назначение ("esp32")
            frame_bytes: Полный кадр в формате RGB565 big-endian (width*height*2 байт)
            width: Ширина кадра в пикселях
            height: Высота кадра в пикселях
            format_code: Код формата (1 = RGB565_BE)
            chunk_rows: Количество строк в одном чанке (по умолчанию 20)

        Returns:
            Ответное сообщение (Dict) либо None при таймауте/ошибке.
        """
        total_size = len(frame_bytes)
        bytes_per_row = width * 2  # RGB565 = 2 байта на пиксель
        header_size = 2 + 2 + 2 + 2 + 2 + 2 + 2  # 7 * u16 = 14 байт

        if chunk_rows <= 0:
            raise ValueError("chunk_rows must be positive")

        if total_size != width * height * 2:
            raise ValueError(f"frame_bytes size mismatch: expected {width * height * 2}, got {total_size}")

        # Количество чанков (по строкам)
        total_chunks = (height + chunk_rows - 1) // chunk_rows

        test_id = str(uuid.uuid4())
        with self.response_lock:
            self.pending_responses[test_id] = {"_received": False}
            self.current_stream_test_id = test_id  # Сохраняем для сопоставления подтверждений

        print(
            f"[client] [STREAM] Отправка кадра на {destination} (потоковый режим с подтверждениями): "
            f"{width}x{height}, bytes={total_size}, chunks={total_chunks} (по {chunk_rows} строк)"
        )

        for chunk_index in range(total_chunks):
            y_start = chunk_index * chunk_rows
            height_chunk = min(chunk_rows, height - y_start)
            
            # Извлекаем данные для этого чанка (height_chunk строк)
            chunk_data_start = y_start * bytes_per_row
            chunk_data_end = (y_start + height_chunk) * bytes_per_row
            chunk_data = frame_bytes[chunk_data_start:chunk_data_end]

            # Внутренний заголовок в начале payload
            inner = bytearray(header_size + len(chunk_data))

            # u16 width
            inner[0:2] = width.to_bytes(2, byteorder="little", signed=False)
            # u16 height
            inner[2:4] = height.to_bytes(2, byteorder="little", signed=False)
            # u16 chunk_index
            inner[4:6] = chunk_index.to_bytes(2, byteorder="little", signed=False)
            # u16 total_chunks
            inner[6:8] = total_chunks.to_bytes(2, byteorder="little", signed=False)
            # u16 format_code
            inner[8:10] = format_code.to_bytes(2, byteorder="little", signed=False)
            # u16 y_start
            inner[10:12] = y_start.to_bytes(2, byteorder="little", signed=False)
            # u16 height_chunk
            inner[12:14] = height_chunk.to_bytes(2, byteorder="little", signed=False)

            # Данные чанка сразу после заголовка
            inner[header_size:] = chunk_data

            chunk_b64 = image_to_base64(bytes(inner))
            
            # Регистрируем ожидание подтверждения этого чанка
            chunk_ack_key = f"{test_id}:{chunk_index}"
            with self.response_lock:
                self.pending_chunk_acks[chunk_ack_key] = False
            
            # Отправляем чанк
            self._send_stream_chunk(
                destination=destination,
                stream_id=self.STREAM_ID_LCD_FRAME,
                seq=chunk_index,
                chunk_payload_b64=chunk_b64,
                test_id=test_id,
                extra_fields=None,
                verbose=True,
            )
            
            # Ждём подтверждения приёма этого чанка (flow control)
            # ВАЖНО: Это критически важно для предотвращения переполнения TCP буферов на win-x64
            # ESP32 может обрабатывать чанки медленнее, чем Python их отправляет
            # Без flow control TCP буфер переполняется (WSAEWOULDBLOCK ошибка)
            print(f"[client] [STREAM] Ожидание подтверждения для чанка {chunk_index}...")
            chunk_ack_timeout = 5.0  # 5 секунд на чанк (можно увеличить для медленных сетей)
            start_time = time.time()
            ack_received = False
            
            while time.time() - start_time < chunk_ack_timeout:
                with self.response_lock:
                    if chunk_ack_key in self.pending_chunk_acks and self.pending_chunk_acks[chunk_ack_key]:
                        ack_received = True
                        del self.pending_chunk_acks[chunk_ack_key]
                        break
                time.sleep(0.05)  # Проверяем каждые 50 мс (не блокируем поток приёма)
            
            if not ack_received:
                # ВАЖНО: Даже при таймауте продолжаем отправку следующих чанков
                # Это позволяет системе восстановиться, если один чанк потерян
                print(f"[client] [STREAM] ⚠️ Таймаут подтверждения для чанка {chunk_index}, продолжаем...")
                with self.response_lock:
                    if chunk_ack_key in self.pending_chunk_acks:
                        del self.pending_chunk_acks[chunk_ack_key]
            else:
                print(f"[client] [STREAM] ✅ Подтверждение получено для чанка {chunk_index}")

        print(
            f"[client] [STREAM] Все чанки отправлены, ожидаем подтверждение (test_id={test_id})"
        )

        # Очищаем текущий test_id потока
        with self.response_lock:
            self.current_stream_test_id = None
        
        # Ждём финальное подтверждение (LCD_FRAME_OK)
        response = self.wait_for_response(test_id, timeout=self.timeout)
        if response is None:
            print("[client] [STREAM] Не получено финальное подтверждение для LCD кадра")
        else:
            print(f"[client] [STREAM] Получен финальный ответ на LCD кадр: {response}")
        return response

    def play_audio_file_to_esp32_dyn(
        self,
        file_path: str,
        chunk_samples: int = 512,
        pace: bool = True,
        pace_factor: float = 0.97,
        command_timeout: float = 15.0,
    ) -> bool:
        """
        Воспроизведение на динамик (MAX98357A): dyn.on → STREAM stream_id=3 (PCM24 mono 48 kHz)
        → dyn.off. Чанки без test_id, чтобы не ломать маршрутизацию ответов на win-x64.

        Требуются: numpy, soundfile; при несовпадении частоты дискретизации — scipy.
        """
        import time
        from pathlib import Path

        import numpy as np

        try:
            import soundfile as sf
        except ImportError as e:
            print("[client] Нужен soundfile: pip install soundfile", file=sys.stderr)
            raise e

        path = Path(file_path).expanduser()
        if not path.is_file():
            print(f"[client] Файл не найден: {path}")
            return False

        audio, sr = sf.read(str(path), always_2d=False, dtype="float64")
        if audio.ndim > 1:
            audio = np.mean(audio, axis=1)

        target_sr = 48000
        if int(sr) != target_sr:
            try:
                from scipy import signal

                n_out = max(1, int(round(audio.shape[0] * target_sr / float(sr))))
                audio = signal.resample(audio, n_out)
                sr = target_sr
                print(f"[client] Ресэмплинг → {target_sr} Hz (scipy)")
            except ImportError:
                print(
                    f"[client] Файл {sr} Hz, нужен 48000 Hz. Установите scipy или конвертируйте файл.",
                    file=sys.stderr,
                )
                return False

        # float [-1,1] → int24
        clip = np.clip(audio, -1.0, 1.0)
        samples_i32 = (clip * (2**23 - 1.0)).astype(np.int32)
        n_total = samples_i32.size
        if n_total == 0:
            print("[client] Пустой файл")
            return False

        if chunk_samples < 1 or chunk_samples > 512:
            raise ValueError("chunk_samples must be 1..512")

        tid_on = self.send_command("esp32", "dyn.on")
        resp_on = self.wait_for_response(tid_on, timeout=command_timeout)
        if not resp_on:
            print("[client] Нет ответа на dyn.on")
            return False

        seq = 0
        for start in range(0, n_total, chunk_samples):
            block = samples_i32[start : start + chunk_samples]
            payload = self.pack_pcm24_stream_payload_mono(block, sample_rate_hz=target_sr)
            chunk_b64 = image_to_base64(payload)
            self._send_stream_chunk(
                destination="esp32",
                stream_id=self.STREAM_ID_DYN_PCM,
                seq=seq,
                chunk_payload_b64=chunk_b64,
                test_id=None,
                extra_fields=None,
                verbose=False,
            )
            seq += 1
            if pace and block.size > 0:
                delay = (block.size / float(target_sr)) * pace_factor
                time.sleep(delay)

        # Очередь на ESP32 и TCP — дождаться drain перед dyn.off, иначе ответ теряется
        time.sleep(0.35)

        tid_off = self.send_command("esp32", "dyn.off")
        resp_off = self.wait_for_response(tid_off, timeout=command_timeout)
        if not resp_off:
            print("[client] Предупреждение: нет ответа на dyn.off")
            return False
        print("[client] Воспроизведение завершено (dyn.off OK)")
        return True

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
            destination: Назначение ("win", "esp32")
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
    
    def send_command(self, destination: str, command: str, payload_data: Optional[Dict] = None, test_id: Optional[str] = None) -> str:
        """
        Отправляет команду на указанное назначение.
        
        Args:
            destination: Назначение ("win", "esp32")
            command: Текст команды (например, "TEXT_CLEAR", "TEXT_ADD")
            payload_data: Дополнительные данные команды в виде словаря (опционально)
            test_id: Уникальный ID команды (генерируется автоматически если не указан)
        
        Returns:
            str: test_id для отслеживания ответа
        """
        if test_id is None:
            test_id = str(uuid.uuid4())
        
        # Формируем payload команды
        if payload_data:
            # Если есть дополнительные данные, формируем JSON строку
            command_payload = json.dumps({
                "command": command,
                **payload_data
            }, ensure_ascii=False)
        else:
            # Просто текст команды
            command_payload = command
        
        # Конвертируем в base64
        command_b64 = image_to_base64(command_payload.encode('utf-8'))
        
        # Формируем JSON сообщение типа "command"
        message = {
            "type": "command",
            "source": "external",
            "destination": destination,
            "priority": 128,
            "stream_id": 0,
            "payload": command_b64,
            "seq": 0,
            "test_id": test_id
        }
        
        # Регистрируем ожидание ответа
        with self.response_lock:
            self.pending_responses[test_id] = {'_received': False}
        
        # Отправляем сообщение
        json_str = json.dumps(message, ensure_ascii=False)
        self.sock.sendto(json_str.encode('utf-8'), (self.server_host, self.server_port))
        
        print(f"[client] Отправлена команда '{command}' test_id={test_id} для destination={destination}")
        if payload_data:
            print(f"[client] Payload размер: {len(command_payload)} байт (base64: {len(command_b64)} символов)")
        
        return test_id
    
    def close(self):
        """Закрывает соединение"""
        self.running = False
        if self.sock:
            self.sock.close()
        print("[client] Клиент закрыт")
