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

void cmd_stm_proc(uint32_t dlen, const char* data)
{
	(void)dlen;
	(void)data;
	return;
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

void cmd_proc(uint32_t len, const char* msg)
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
char tx1msg[256];
char rx1_char[1] = {};
uint32_t rxusart1buf_i = 0;
volatile char txusart1buf[TXBUFUSART1] = {};
volatile char rxusart1buf[RXBUFUSART1] = {};
const uint8_t rxchar_lag = 2;
const char msg[] = "zdarova";
const int msg_size = 7;
void rxusart1_response(uint8_t is_terminal)
{
	int msg_size_ = 0;
	uint8_t is_msg = 0;
	if ((memcmp(rxusart1buf, "privet", 6) == 0) && (rxusart1buf_i == 6))
	{
		is_msg = 1;
		msg_size_ = msg_size;
		memcpy(txusart1buf, msg, msg_size);
	}
	else if ((memcmp(rxusart1buf, "gorbunki", 8) == 0) && (rxusart1buf_i == 8))
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
	
	HAL_UART_Receive_IT(&huart1, rxusart1buf+rxusart1buf_i, 1);
}


#define RXBUFUSART2 128
#define TXBUFUSART2 128
char tx2msg[256];
char rx2_char[1] = {};
uint32_t rxusart2buf_i = 0;
volatile char txusart2buf[TXBUFUSART2] = {};
volatile char rxusart2buf[RXBUFUSART2] = {};
void rxusart2_response(uint8_t is_terminal)
{
	int msg_size_ = 0;
	uint8_t is_msg = 0;
	if ((memcmp(rxusart2buf, "privet", 6) == 0) && (rxusart2buf_i == 6))
	{
		is_msg = 1;
		msg_size_ = msg_size;
		memcpy(txusart2buf, msg, msg_size);
	}
	else if ((memcmp(rxusart2buf, "gorbunki", 8) == 0) && (rxusart2buf_i == 8))
	{
		is_msg = 1;
		msg_size_ = 11;
		memcpy(txusart2buf, "Liquidation", 11);
	}
	else
	{
		msg_size_ = 0;
	}
	
	if (is_terminal) memcpy(tx2msg, crlf, 2); // end current input line
	if (is_msg)
	{
		memcpy(tx2msg+(is_terminal ? 2 : 0), txusart2buf, msg_size_); // output line
		memcpy(tx2msg+(is_terminal ? 2 : 0)+msg_size_, crlf, 2); // end output line
		msg_size_ += (is_terminal ? 2 : 0);
	}
	if (is_terminal) memcpy(tx2msg+(is_terminal ? 2 : 0)+msg_size_, lbarr, 2); // begin next input line
	
	HAL_UART_Transmit(&huart2, tx2msg, msg_size_+(is_terminal ? 4 : 0), HAL_MAX_DELAY);
}

uint8_t rx2char_lag = 0x87;
char txconst[1] = { 0x55 };
void uart2rx_cb(void)
{
	HAL_UART_Transmit(&huart2, rx2_char+rxusart2buf_i+rx2char_lag, 1, HAL_MAX_DELAY);
	
	if (rx2_char[rxusart2buf_i+rx2char_lag] == 0x0D)
	{
		rxusart2_response(0);
		rxusart2buf_i = 0;
	}
	else
	{
		++rxusart2buf_i;
	}
	
	HAL_UART_Receive_IT(&huart2, rxusart2buf+rxusart2buf_i, 1);
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
	HAL_UART_Receive_IT(&huart1, rxusart1buf, 1);
	HAL_UART_Receive_IT(&huart2, rxusart2buf, 1);
	//HAL_UART_Transmit_IT(&huart1,txusart1buf,strlen(txusart1buf));
	HAL_TIM_Base_Start_IT(&htim2);
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
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
