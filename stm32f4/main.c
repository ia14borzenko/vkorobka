#include "stm32f4xx.h"
#include <string.h>

// Function prototypes
void SystemClock_Conf(void);
void USART1_Init(void);
void USART2_Init(void);
void DMA_Config(void);
void TIM2_Init(void);

// DMA functions for data streams
void DMA_USART1_TX(uint8_t* data, uint32_t len);
void DMA_USART1_RX(uint8_t* buffer, uint32_t len);
void DMA_USART2_TX(uint8_t* data, uint32_t len);
void DMA_USART2_RX(uint8_t* buffer, uint32_t len);

// Buffer for USART2 RX (terminal input)
#define RX_BUFFER_SIZE 256
uint8_t rx_buffer[RX_BUFFER_SIZE];
volatile uint32_t rx_index = 0;  // Current position in buffer
volatile uint32_t last_ndtr = RX_BUFFER_SIZE;
volatile uint8_t new_data_available = 0;  // Flag for new data

// Example transmit function for terminal
void Send_Message_USART2(const uint8_t* data, uint32_t len);

// Main function
int main(void) {
    SystemClock_Conf();
    USART1_Init();
    USART2_Init();
    DMA_Config();
    TIM2_Init();
    
    // Start DMA RX for USART2 (continuous reception for terminal)
    DMA_USART2_RX(rx_buffer, RX_BUFFER_SIZE);
    
    // Enable TIM2
    TIM2->CR1 |= TIM_CR1_CEN;
    
    // Example: Send a welcome message via USART2
    // Send_Message_USART2("Welcome to STM32 Terminal\r\n");
		Send_Message_USART2((const uint8_t*)"Welcome to STM32 Terminal\r\n", strlen("Welcome to STM32 Terminal\r\n"));
    
    while(1) {
        // Main loop can handle other tasks
        // Terminal input is processed in TIM2 IRQ
    }
}

// System Clock Configuration
void SystemClock_Conf(void) {
    // Enable HSE
    RCC->CR |= RCC_CR_HSEON;
    while (!(RCC->CR & RCC_CR_HSERDY));
    
    // Configure PLL: PLLM = 25, PLLN = 168, PLLP = 2
    RCC->PLLCFGR = (25 << RCC_PLLCFGR_PLLM_Pos) | (168 << RCC_PLLCFGR_PLLN_Pos) | (0 << RCC_PLLCFGR_PLLP_Pos) | RCC_PLLCFGR_PLLSRC_HSE;
    
    // Enable PLL
    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY));
    
    // Set system clock to PLL
    RCC->CFGR |= RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL);
    
    // AHB Prescaler /1
    RCC->CFGR &= ~RCC_CFGR_HPRE;
    
    // APB1 Prescaler /2 (peripheral 42MHz, timer 84MHz)
    RCC->CFGR |= RCC_CFGR_PPRE1_DIV2;
    
    // APB2 Prescaler /1 (84MHz)
    RCC->CFGR &= ~RCC_CFGR_PPRE2;
}

// USART1 Initialization
void USART1_Init(void) {
    // Enable clock for USART1 and GPIOA
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    
    // GPIOA9 (TX), GPIOA10 (RX) alternate function 7
    GPIOA->AFR[1] |= (7 << GPIO_AFRH_AFSEL9_Pos) | (7 << GPIO_AFRH_AFSEL10_Pos);
    GPIOA->MODER |= GPIO_MODER_MODER9_1 | GPIO_MODER_MODER10_1;
    
    // USART config: 1152000 baud, 8N1, oversampling 16
    USART1->BRR = SystemCoreClock / 1152000;  // Assuming 84MHz APB2
    USART1->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
    USART1->CR2 = 0;  // Stop bits 1
    USART1->CR3 = 0;  // No parity
}

// USART2 Initialization (similar for terminal)
void USART2_Init(void) {
    // Enable clock for USART2 and GPIOA
    RCC->APB1ENR |= RCC_APB1ENR_USART2EN;
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    
    // GPIOA2 (TX), GPIOA3 (RX) alternate function 7
    GPIOA->AFR[0] |= (7 << GPIO_AFRL_AFSEL2_Pos) | (7 << GPIO_AFRL_AFSEL3_Pos);
    GPIOA->MODER |= GPIO_MODER_MODER2_1 | GPIO_MODER_MODER3_1;
    
    // USART config: 1152000 baud, 8N1, oversampling 16
    USART2->BRR = (SystemCoreClock / 2) / 1152000;  // APB1 peripheral 42MHz
    USART2->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
    USART2->CR2 = 0;
    USART2->CR3 = 0;
}

// DMA Configuration
void DMA_Config(void) {
    // Enable DMA clocks
    RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN | RCC_AHB1ENR_DMA2EN;
    
    // USART1 RX: DMA2 Stream2 Channel4, per to mem DIR=00
    DMA2_Stream2->CR = (4 << DMA_SxCR_CHSEL_Pos) | DMA_SxCR_MINC | DMA_SxCR_TCIE | DMA_SxCR_HTIE | DMA_SxCR_TEIE | DMA_SxCR_DMEIE;
    DMA2_Stream2->PAR = (uint32_t)&USART1->DR;
    DMA2_Stream2->FCR = DMA_SxFCR_DMDIS | (3 << DMA_SxFCR_FTH_Pos);  // FIFO mode
    
    // USART1 TX: DMA2 Stream7 Channel4, mem to per DIR=01
    DMA2_Stream7->CR = (4 << DMA_SxCR_CHSEL_Pos) | DMA_SxCR_MINC | DMA_SxCR_DIR_0 | DMA_SxCR_TCIE | DMA_SxCR_HTIE | DMA_SxCR_TEIE | DMA_SxCR_DMEIE;
    DMA2_Stream7->PAR = (uint32_t)&USART1->DR;
    DMA2_Stream7->FCR = DMA_SxFCR_DMDIS | (3 << DMA_SxFCR_FTH_Pos);
    
    // USART2 RX: DMA1 Stream5 Channel4, per to mem DIR=00
    DMA1_Stream5->CR = (4 << DMA_SxCR_CHSEL_Pos) | DMA_SxCR_MINC | DMA_SxCR_TCIE | DMA_SxCR_HTIE | DMA_SxCR_TEIE | DMA_SxCR_DMEIE;
    DMA1_Stream5->PAR = (uint32_t)&USART2->DR;
    DMA1_Stream5->FCR = DMA_SxFCR_DMDIS | (3 << DMA_SxFCR_FTH_Pos);
    
    // USART2 TX: DMA1 Stream6 Channel4, mem to per DIR=01
    DMA1_Stream6->CR = (4 << DMA_SxCR_CHSEL_Pos) | DMA_SxCR_MINC | DMA_SxCR_DIR_0 | DMA_SxCR_TCIE | DMA_SxCR_HTIE | DMA_SxCR_TEIE | DMA_SxCR_DMEIE;
    DMA1_Stream6->PAR = (uint32_t)&USART2->DR;
    DMA1_Stream6->FCR = DMA_SxFCR_DMDIS | (3 << DMA_SxFCR_FTH_Pos);
    
    // Enable DMA for USARTs
    USART1->CR3 |= USART_CR3_DMAT | USART_CR3_DMAR;
    USART2->CR3 |= USART_CR3_DMAT | USART_CR3_DMAR;
    
    // NVIC for DMA interrupts (optional, depending on needs)
    // NVIC_EnableIRQ(DMA2_Stream2_IRQn); etc.
}

// DMA USART1 TX
void DMA_USART1_TX(uint8_t* data, uint32_t len) {
    DMA2_Stream7->M0AR = (uint32_t)data;
    DMA2_Stream7->NDTR = len;
    DMA2_Stream7->CR |= DMA_SxCR_EN;
}

// DMA USART1 RX
void DMA_USART1_RX(uint8_t* buffer, uint32_t len) {
    DMA2_Stream2->M0AR = (uint32_t)buffer;
    DMA2_Stream2->NDTR = len;
    DMA2_Stream2->CR |= DMA_SxCR_EN;
}

// DMA USART2 TX
void DMA_USART2_TX(uint8_t* data, uint32_t len) {
    DMA1_Stream6->M0AR = (uint32_t)data;
    DMA1_Stream6->NDTR = len;
    DMA1_Stream6->CR |= DMA_SxCR_EN;
}

// DMA USART2 RX (circular for continuous)
void DMA_USART2_RX(uint8_t* buffer, uint32_t len) {
    DMA1_Stream5->M0AR = (uint32_t)buffer;
    DMA1_Stream5->NDTR = len;
    DMA1_Stream5->CR |= DMA_SxCR_CIRC | DMA_SxCR_EN;  // Circular mode for continuous RX
}

// Send message via USART2
void Send_Message_USART2(const uint8_t* data, uint32_t len) {
    DMA_USART2_TX((uint8_t*)data, len);
}

// TIM2 Initialization (5ms interrupt)
void TIM2_Init(void) {
    RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;
    
    // Prescaler: APB1 timer clock 84MHz, /84 =1MHz
    TIM2->PSC = 83;
    // ARR for 5ms: 1MHz / 200 =5ms, ARR=5000-1=4999
    TIM2->ARR = 4999;
    TIM2->DIER |= TIM_DIER_UDE;
    TIM2->CR1 |= TIM_CR1_URS;
    
    NVIC_EnableIRQ(TIM2_IRQn);
}

// TIM2 IRQ Handler: Check received data every 5ms
void TIM2_IRQHandler(void) {
    if (TIM2->SR & TIM_SR_UIF) {
        TIM2->SR &= ~TIM_SR_UIF;
        
        // Check DMA RX position for USART2
        uint32_t curr_ndtr = DMA1_Stream5->NDTR;
        int32_t delta = last_ndtr - curr_ndtr;
        if (delta < 0) {
            delta += RX_BUFFER_SIZE;
        }
        if (delta > 0) {
            new_data_available = 1;
            
            // Process data (example: echo back the new bytes, handling wrap-around)
            uint32_t to_send = delta;
            uint32_t start = rx_index;
            while (to_send > 0) {
                uint32_t chunk = (to_send > RX_BUFFER_SIZE - start) ? (RX_BUFFER_SIZE - start) : to_send;
                Send_Message_USART2((const uint8_t*)&rx_buffer[start], chunk);
                start = 0;
                to_send -= chunk;
            }
            
            // Update rx_index
            rx_index = (rx_index + delta) % RX_BUFFER_SIZE;
        }
        last_ndtr = curr_ndtr;
    }
}