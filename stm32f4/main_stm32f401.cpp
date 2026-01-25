#include "stm32f401xc.h"
#include <cstring>

static constexpr uint32_t BAUDRATE = 3000000;
static constexpr uint32_t RX_BUF_SZ = 512;

alignas(4) uint8_t uart1_rx_buf[RX_BUF_SZ];
alignas(4) uint8_t uart1_tx_buf[RX_BUF_SZ];

volatile uint16_t rx_head = 0;

static void clock_init()
{
    RCC->CR |= RCC_CR_HSEON;
    while (!(RCC->CR & RCC_CR_HSERDY));

		RCC->PLLCFGR =
				(8  << RCC_PLLCFGR_PLLM_Pos) |   // 8 MHz / 8 = 1 MHz
				(168 << RCC_PLLCFGR_PLLN_Pos) |  // 1 MHz * 168 = 168 MHz VCO
				(1  << RCC_PLLCFGR_PLLP_Pos) |   // /4 → SYSCLK = 84 MHz
				RCC_PLLCFGR_PLLSRC_HSE |
				(7  << RCC_PLLCFGR_PLLQ_Pos);


    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY));

    FLASH->ACR = FLASH_ACR_ICEN | FLASH_ACR_DCEN | FLASH_ACR_LATENCY_2WS;

    RCC->CFGR |= RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL);

    RCC->CFGR |= RCC_CFGR_PPRE1_DIV2;
}

static void gpio_init()
{
    /* GPIOA — UART */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;

    GPIOA->MODER |=
        (2 << GPIO_MODER_MODER9_Pos) |
        (2 << GPIO_MODER_MODER10_Pos) |
        (2 << GPIO_MODER_MODER2_Pos) |
        (2 << GPIO_MODER_MODER3_Pos);

    GPIOA->AFR[1] |= (7 << GPIO_AFRH_AFSEL9_Pos) |
                     (7 << GPIO_AFRH_AFSEL10_Pos);

    GPIOA->AFR[0] |= (7 << GPIO_AFRL_AFSEL2_Pos) |
                     (7 << GPIO_AFRL_AFSEL3_Pos);

    /* GPIOC — LED PC13 */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOCEN;

    GPIOC->MODER &= ~(3u << (13 * 2));
    GPIOC->MODER |=  (1u << (13 * 2));   // Output

    GPIOC->OTYPER &= ~(1u << 13);         // Push-pull
    GPIOC->OSPEEDR &= ~(3u << (13 * 2));  // Low speed
    GPIOC->PUPDR &= ~(3u << (13 * 2));    // No pull

    /* LED ON (активный ноль) */
    GPIOC->BSRR = (1u << (13 + 16));
}

static void usart_init()
{
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
    RCC->APB1ENR |= RCC_APB1ENR_USART2EN;

    /* USART1 — быстрый линк (ESP32) */
    USART1->BRR = SystemCoreClock / BAUDRATE;
    USART1->CR1 = USART_CR1_RE | USART_CR1_TE;
    USART1->CR3 = USART_CR3_DMAT | USART_CR3_DMAR;
    USART1->CR1 |= USART_CR1_UE;

    /* USART2 — FTDI 115200 */
    USART2->BRR = SystemCoreClock / 2 / 115200;   // 42 MHz / 115200
    USART2->CR1 = USART_CR1_RE | USART_CR1_TE;
    USART2->CR1 |= USART_CR1_UE;
}

static void dma_init()
{
    RCC->AHB1ENR |= RCC_AHB1ENR_DMA2EN;

    DMA2_Stream2->CR = 0;
    DMA2_Stream2->PAR  = (uint32_t)&USART1->DR;
    DMA2_Stream2->M0AR = (uint32_t)uart1_rx_buf;
    DMA2_Stream2->NDTR = RX_BUF_SZ;
    DMA2_Stream2->CR =
        DMA_SxCR_MINC |
        DMA_SxCR_CIRC |
        DMA_SxCR_PL_1 |
        DMA_SxCR_CHSEL_2;

    DMA2_Stream2->CR |= DMA_SxCR_EN;
}

static void uart2_putc(char c)
{
    while (!(USART2->SR & USART_SR_TXE));
    USART2->DR = c;
}

static void uart2_write(const char* s)
{
    while (*s) uart2_putc(*s++);
}

int main()
{
    clock_init();
    gpio_init();    // здесь PC13 уже включён
    usart_init();
    dma_init();

    uart2_write("STM32 ready\r\n");

    uint16_t last_pos = 0;

    while (1)
    {
        uint16_t pos = RX_BUF_SZ - DMA2_Stream2->NDTR;
        if (pos != last_pos)
        {
            uint8_t c = uart1_rx_buf[last_pos];
            uart2_putc(c);
            last_pos = (last_pos + 1) % RX_BUF_SZ;
        }
    }
}
