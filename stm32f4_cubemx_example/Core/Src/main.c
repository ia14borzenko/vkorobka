/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "dma.h"
#include "i2s.h"
#include "spi.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <string.h>
#include <stdio.h>
#include "message_protocol.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
void PeriphCommonClock_Config(void);
/* USER CODE BEGIN PFP */

typedef enum
{
	CMD_WIN = 0x0041,
	CMD_ESP = 0x0048,
	CMD_STM = 0x0049,
	
	// one-direction-data devices
	CMD_DPL = 0x00A1, // display
	CMD_MIC = 0x00A2, // microphone
	CMD_SPK = 0x00A3, // speaker
	
} CMD_CODE;

#define CMD_CODE_LEN 2
#define CMD_LEN_LEN 8
#define CMD_HEADER_LEN (CMD_CODE_LEN + CMD_LEN_LEN)

uint8_t got_cmd_begin(uint32_t len, const char* msg)
{
	const uint32_t cmd_header_length = CMD_HEADER_LEN;
	if (len < cmd_header_length)
	{
		return 0;
	}
	return 1;
}

void send_uart(UART_HandleTypeDef *huart, const void* data, uint32_t size)
{
	HAL_UART_Transmit(huart, data, size, HAL_MAX_DELAY);
}

char stm_resp[] = "stm32";
void cmd_stm_proc(uint32_t dlen, const char* data)
{
	if (memcmp(data, "win11", 5))
	{
		send_uart(&huart2, stm_resp, 5);
	}
}

void cmd_display_proc(uint32_t dlen, const char* data)
{
	(void)dlen;
	(void)data;
	return;
}

void cmd_speaker_proc(uint32_t dlen, const char* data)
{
	(void)dlen;
	(void)data;
	return;
}

void cmd_proc(const char* msg, uint32_t len)
{
	if (!got_cmd_begin(len, msg)) return;
	
	const char* data = msg + CMD_HEADER_LEN;
	const uint64_t data_len = ((uint64_t)msg[2] << 0) |
		((uint64_t)msg[3] << (8 * 1)) |
		((uint64_t)msg[3] << (8 * 2)) |
		((uint64_t)msg[3] << (8 * 3)) |
		((uint64_t)msg[3] << (8 * 4)) |
		((uint64_t)msg[3] << (8 * 5)) |
		((uint64_t)msg[3] << (8 * 6)) |
		((uint64_t)msg[3] << (8 * 7));
	
	uint16_t cmd_code = (msg[0]) | (msg[1] << 8);

	switch (cmd_code)
	{
		case (CMD_WIN):  // ignore windows-for
			break;
		case (CMD_ESP):  // ignore esp32-for
			break;
		case (CMD_STM):
			cmd_stm_proc(data_len, data);
			break;
		
		case(CMD_DPL):
			cmd_display_proc(data_len, data);
			break;
		
		case(CMD_MIC): // no to-microphone-directed data
			break;
		
		case(CMD_SPK):
			cmd_speaker_proc(data_len, data);
			break;
	};
	
}
	
	

const char backspace_msg[2] = " \b";
void backspace(uint16_t i, UART_HandleTypeDef *huart)
{
	if (i <= 1)
	{
		i = 0;
		return;
	}
	HAL_UART_Transmit(huart, backspace_msg, 2, HAL_MAX_DELAY);
}
	
const char crlf[2] = {'\r', '\n'};
const char lbarr[2] = "> ";

#define RXBUFUSART1 128
#define TXBUFUSART1 128
char tx1msg[TXBUFUSART1];
char rx1_char[RXBUFUSART1] = {};
uint32_t rxusart1buf_i = 0;
volatile char txusart1buf[TXBUFUSART1] = {};
volatile char rxusart1buf[RXBUFUSART1] = {};
const uint8_t rxchar_lag = 0;
const char msg[] = "zdarova";
const int msg_size = 7;
void rxusart1_response(uint8_t is_terminal)
{
	int msg_size_ = 0;
	uint8_t is_msg = 0;
	if ((memcmp(rx1_char, "privet", 6) == 0) && (rxusart1buf_i == 6))
	{
		is_msg = 1;
		msg_size_ = msg_size;
		memcpy(txusart1buf, msg, msg_size);
	}
	else if ((memcmp(rx1_char, "gorbunki", 8) == 0) && (rxusart1buf_i == 8))
	{
		is_msg = 1;
		msg_size_ = 11;
		memcpy(txusart1buf, "Liquidation", 11);
	}
	else
	{
		msg_size_ = 0;
	}
	
	if (is_terminal) memcpy(tx1msg, crlf, 2); // end current input line
	if (is_msg)
	{
		memcpy(tx1msg+(is_terminal ? 2 : 0), txusart1buf, msg_size_); // output line
		memcpy(tx1msg+(is_terminal ? 2 : 0)+msg_size_, crlf, 2); // end output line
		msg_size_ += (is_terminal ? 2 : 0);
	}
	if (is_terminal) memcpy(tx1msg+(is_terminal ? 2 : 0)+msg_size_, lbarr, 2); // begin next input line
	
	HAL_UART_Transmit(&huart1, tx1msg, msg_size_+(is_terminal ? 4 : 0), HAL_MAX_DELAY);
}

void uart1rx_cb(void)
{
	HAL_UART_Transmit(&huart1, rx1_char+rxusart1buf_i+rxchar_lag, 1, HAL_MAX_DELAY);
	
	if (rx1_char[rxusart1buf_i+rxchar_lag] == 0x0D)
	{
		rxusart1_response(1);
		rxusart1buf_i = 0;
	}
	else if (rx1_char[rxusart1buf_i+rxchar_lag] == 0x08)
	{
		backspace(rxusart1buf_i, &huart1);
		--rxusart1buf_i;
	}
	else
	{
		++rxusart1buf_i;
	}
	
	HAL_UART_Receive_IT(&huart1, rx1_char+rxusart1buf_i, 1);
}


#define RXBUFUSART2 128
#define TXBUFUSART2 128
char tx2msg[256];
char rx2_char[RXBUFUSART2] = {};
uint32_t rxusart2buf_i = 0;
volatile char txusart2buf[TXBUFUSART2] = {};

// Буфер для накопления данных нового протокола message_protocol
#define MSG_PROTOCOL_BUFFER_SIZE 4096
static uint8_t msg_protocol_buffer[MSG_PROTOCOL_BUFFER_SIZE] = {};
static uint32_t msg_protocol_buf_pos = 0;

uint8_t rx2char_lag = 0x0;
char txconst[1] = { 0x55 };

// Функция для вывода заголовка на UART1 (отладчик)
void print_header_to_uart1(const msg_header_t* header, const uint8_t* payload, uint32_t payload_len)
{
	char debug_msg[256];
	int len = 0;
	
	len += sprintf(debug_msg + len, "[TEST RX] type=%d src=%d dst=%d len=%lu seq=%d\r\n",
		header->msg_type, header->source_id, header->destination_id, 
		(unsigned long)header->payload_len, header->sequence);
	
	// Отладочный вывод: показываем байты заголовка из буфера
	len += sprintf(debug_msg + len, "[TEST RX] Header bytes: ");
	for (int i = 0; i < MSG_HEADER_LEN && i < 12; i++)
	{
		len += sprintf(debug_msg + len, "%02X ", msg_protocol_buffer[i]);
	}
	len += sprintf(debug_msg + len, "\r\n");
	
	// Выводим информацию о реальном payload
	if (payload != NULL && payload_len > 0)
	{
		len += sprintf(debug_msg + len, "[TEST RX] Payload received: %lu bytes\r\n", (unsigned long)payload_len);
		if (payload_len <= 16)
		{
			len += sprintf(debug_msg + len, "[TEST RX] Payload data: ");
			for (uint32_t i = 0; i < payload_len; i++)
			{
				len += sprintf(debug_msg + len, "%02X ", payload[i]);
			}
			len += sprintf(debug_msg + len, "\r\n");
		}
	}
	else
	{
		len += sprintf(debug_msg + len, "[TEST RX] WARNING: No payload data (payload_len=%lu)\r\n", 
			(unsigned long)payload_len);
	}
	
	HAL_UART_Transmit(&huart1, (uint8_t*)debug_msg, len, HAL_MAX_DELAY);
}

// Union для работы с заголовком как с байтами и структурой
typedef union
{
	msg_header_t header;
	uint8_t bytes[MSG_HEADER_LEN];
} msg_header_union_t;

// Обработка нового протокола message_protocol
void process_message_protocol(void)
{
	// Обрабатываем все доступные пакеты в буфере
	while (msg_protocol_buf_pos >= MSG_HEADER_LEN)
	{
		// Используем union для прямого доступа к байтам
		msg_header_union_t header_union;
		
		// Копируем заголовок из буфера
		memcpy(header_union.bytes, msg_protocol_buffer, MSG_HEADER_LEN);
		
		// Читаем поля напрямую из байтов (little-endian)
		msg_header_t header;
		header.msg_type = header_union.bytes[0];
		header.source_id = header_union.bytes[1];
		header.destination_id = header_union.bytes[2];
		header.route_flags = header_union.bytes[3];
		header.priority = header_union.bytes[4];
		
		// Stream ID (2 bytes, little-endian): [5] = младший, [6] = старший
		header.stream_id = (uint16_t)(header_union.bytes[5] | (header_union.bytes[6] << 8));
		
		// Payload length (4 bytes, little-endian): [7] = младший, [10] = старший
		header.payload_len = (uint32_t)(header_union.bytes[7] | 
		                                (header_union.bytes[8] << 8) | 
		                                (header_union.bytes[9] << 16) | 
		                                (header_union.bytes[10] << 24));
		
		// Sequence number (1 byte)
		header.sequence = header_union.bytes[11];
		
		// Отладочный вывод для диагностики - показываем что реально в буфере
		char debug_buf[256];
		int dbg_len = sprintf(debug_buf, "[DEBUG] Buffer[0-11]: ");
		for (int i = 0; i < 12 && i < MSG_HEADER_LEN; i++)
		{
			dbg_len += sprintf(debug_buf + dbg_len, "%02X ", msg_protocol_buffer[i]);
		}
		dbg_len += sprintf(debug_buf + dbg_len, "\r\n");
		dbg_len += sprintf(debug_buf + dbg_len, "[DEBUG] Parsed: payload_len=%lu from bytes[7-10]=%02X %02X %02X %02X\r\n",
			(unsigned long)header.payload_len,
			header_union.bytes[7], header_union.bytes[8], 
			header_union.bytes[9], header_union.bytes[10]);
		dbg_len += sprintf(debug_buf + dbg_len, "[DEBUG] Buffer size: %lu bytes\r\n", (unsigned long)msg_protocol_buf_pos);
		HAL_UART_Transmit(&huart1, (uint8_t*)debug_buf, dbg_len, HAL_MAX_DELAY);
		
		// Проверяем валидность заголовка
		if (!msg_validate_header(&header))
		{
			// Невалидный заголовок, пропускаем один байт
			memmove(msg_protocol_buffer, msg_protocol_buffer + 1, msg_protocol_buf_pos - 1);
			msg_protocol_buf_pos--;
			continue;
		}
		
		// Проверяем, есть ли полный пакет
		uint32_t total_packet_size = MSG_HEADER_LEN + header.payload_len;
		if (msg_protocol_buf_pos < total_packet_size)
		{
			// Недостаточно данных, ждем еще
			break;
		}
		
		// Полный пакет получен
		// Получаем указатель на payload
		const uint8_t* payload = NULL;
		uint32_t payload_len = 0;
		
		if (header.payload_len > 0)
		{
			payload = msg_protocol_buffer + MSG_HEADER_LEN;
			payload_len = header.payload_len;
		}
		
		// Проверяем, это тест для STM32?
		if (header.destination_id == MSG_DST_STM32)
		{
			// Выводим заголовок и payload на UART1 (отладчик)
			print_header_to_uart1(&header, payload, payload_len);
		}
			
		// Удаляем обработанный пакет из буфера
		memmove(msg_protocol_buffer, msg_protocol_buffer + total_packet_size, 
			msg_protocol_buf_pos - total_packet_size);
		msg_protocol_buf_pos -= total_packet_size;
		
		// Защита от переполнения
		if (msg_protocol_buf_pos >= MSG_PROTOCOL_BUFFER_SIZE - 1)
		{
			// Буфер переполнен, очищаем
			msg_protocol_buf_pos = 0;
			break;
		}
	}
}

void uart2rx_cb(void)
{
	uint8_t received_byte = rx2_char[rxusart2buf_i + rx2char_lag];
	
	// Добавляем байт в буфер нового протокола (если есть место)
	if (msg_protocol_buf_pos < MSG_PROTOCOL_BUFFER_SIZE - 1)
	{
		msg_protocol_buffer[msg_protocol_buf_pos] = received_byte;
		msg_protocol_buf_pos++;
		
		// Пытаемся обработать новый протокол
		process_message_protocol();
	}
	
	// Старая логика для совместимости
	HAL_UART_Transmit(&huart2, rx2_char+rxusart2buf_i+rx2char_lag, 1, HAL_MAX_DELAY);
	
	if (rx2_char[rxusart2buf_i+rx2char_lag] == 0x0D)
	{
		cmd_proc(rx2_char, rxusart2buf_i);
		rxusart2buf_i = 0;
	}
	else
	{
		++rxusart2buf_i;
	}
	
	HAL_UART_Receive_IT(&huart2, rx2_char+rxusart2buf_i, 1);
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
	if (huart == &huart1)
	{
		uart1rx_cb();
	}
	else if (huart == &huart2)
	{
		uart2rx_cb();
	}
}
	


	
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
	
}

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* Configure the peripherals common clocks */
  PeriphCommonClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_USART1_UART_Init();
  MX_USART2_UART_Init();
  MX_TIM2_Init();
  MX_I2S2_Init();
  MX_I2S3_Init();
  MX_SPI1_Init();
  /* USER CODE BEGIN 2 */
	HAL_UART_Receive_IT(&huart1, rx1_char, 1);
	HAL_UART_Receive_IT(&huart2, rx2_char, 1);
	//HAL_UART_Transmit_IT(&huart1,txusart1buf,strlen(txusart1buf));
	HAL_TIM_Base_Start_IT(&htim2);
	
	// Пример использования C++ классов через C-обертки:
	// #include "uart_handler_c_wrapper.h"
	// #include "message_queue_c_wrapper.h"
	// 
	// static uart_handler_handle_t g_uart_handler = NULL;
	// static message_queue_handle_t g_message_queue = NULL;
	// 
	// // Callback для обработки сообщений
	// void handle_message_callback(const msg_header_t* header, const u8* payload, u32 payload_len)
	// {
	//     if (g_message_queue && header)
	//     {
	//         message_queue_enqueue(g_message_queue, header, payload, payload_len);
	//     }
	// }
	// 
	// // Инициализация
	// g_uart_handler = uart_handler_create();
	// if (g_uart_handler)
	// {
	//     uart_handler_init(g_uart_handler, 2, 115200);  // USART2
	//     uart_handler_start(g_uart_handler);
	//     uart_handler_register_type_handler(g_uart_handler, MSG_TYPE_COMMAND, handle_message_callback);
	//     uart_handler_register_type_handler(g_uart_handler, MSG_TYPE_DATA, handle_message_callback);
	// }
	// 
	// g_message_queue = message_queue_create();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    // Пример обработки UART данных и очереди сообщений:
    // if (g_uart_handler)
    // {
    //     uart_handler_process_rx_data(g_uart_handler);
    // }
    // 
    // if (g_message_queue && !message_queue_empty(g_message_queue))
    // {
    //     msg_header_t header;
    //     u8 payload_buffer[1024];
    //     u32 payload_len = 0;
    //     
    //     if (message_queue_dequeue(g_message_queue, &header, payload_buffer, 
    //                               sizeof(payload_buffer), &payload_len))
    //     {
    //         // Обработка сообщения
    //         switch (header.msg_type)
    //         {
    //             case MSG_TYPE_COMMAND:
    //                 // Обработка команд
    //                 break;
    //             case MSG_TYPE_DATA:
    //                 // Обработка данных
    //                 break;
    //             default:
    //                 break;
    //         }
    //     }
    // }
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE2);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 25;
  RCC_OscInitStruct.PLL.PLLN = 168;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief Peripherals Common Clock Configuration
  * @retval None
  */
void PeriphCommonClock_Config(void)
{
  RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};

  /** Initializes the peripherals clock
  */
  PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_I2S;
  PeriphClkInitStruct.PLLI2S.PLLI2SN = 192;
  PeriphClkInitStruct.PLLI2S.PLLI2SR = 2;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
