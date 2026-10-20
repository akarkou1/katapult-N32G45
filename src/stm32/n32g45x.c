// Code to setup clocks and gpio on N32G45x
//
// Distributed under the terms of the GNU GPLv3 license.

#include "autoconf.h" // CONFIG_CLOCK_REF_FREQ
#include "board/armcm_boot.h" // VectorTable
#include "board/armcm_reset.h" // try_request_canboot
#include "board/irq.h" // irq_disable
#include "board/misc.h" // bootloader_request
#include "internal.h" // enable_pclock
#include "sched.h" // sched_main

/****************************************************************
 * Clock setup
 ****************************************************************/

#define FREQ_APB1 (CONFIG_CLOCK_FREQ / 4) // Max 36 MHz at 144 MHz SYSCLK
#define FREQ_APB2 (CONFIG_CLOCK_FREQ / 2) // Max 72 MHz at 144 MHz SYSCLK

// Map a peripheral address to its enable and reset bits
struct cline
lookup_clock_line(uint32_t periph_base)
{
    if (periph_base >= AHBPERIPH_BASE) {
        if (periph_base == SDIO_BASE)
            return (struct cline){.en=&RCC->AHBPCLKEN, .bit=RCC_AHBPCLKEN_SDIOEN};
        if (periph_base == CRC_BASE)
            return (struct cline){.en=&RCC->AHBPCLKEN, .bit=RCC_AHBPCLKEN_CRCEN};
        uint32_t bit = 1 << ((periph_base - DMA1_BASE) / 0x400);
        return (struct cline){.en=&RCC->AHBPCLKEN, .rst=&RCC->AHBPRST, .bit=bit};
    } else if (periph_base >= APB2PERIPH_BASE) {
        if (periph_base == DVP_BASE)
            return (struct cline){.en=&RCC->APB2PCLKEN, .rst=&RCC->APB2PRST, .bit=RCC_APB2PCLKEN_DVPEN};
        if (periph_base == UART6_BASE)
            return (struct cline){.en=&RCC->APB2PCLKEN, .rst=&RCC->APB2PRST, .bit=RCC_APB2PCLKEN_UART6EN};
        if (periph_base == UART7_BASE)
            return (struct cline){.en=&RCC->APB2PCLKEN, .rst=&RCC->APB2PRST, .bit=RCC_APB2PCLKEN_UART7EN};
        if (periph_base == I2C3_BASE)
            return (struct cline){.en=&RCC->APB2PCLKEN, .rst=&RCC->APB2PRST, .bit=RCC_APB2PCLKEN_I2C3EN};
        if (periph_base == I2C4_BASE)
            return (struct cline){.en=&RCC->APB2PCLKEN, .rst=&RCC->APB2PRST, .bit=RCC_APB2PCLKEN_I2C4EN};
        uint32_t bit = 1 << ((periph_base - APB2PERIPH_BASE) / 0x400);
        return (struct cline){.en=&RCC->APB2PCLKEN, .rst=&RCC->APB2PRST, .bit=bit};
    } else {
        uint32_t bit = 1 << ((periph_base - APB1PERIPH_BASE) / 0x400);
        return (struct cline){.en=&RCC->APB1PCLKEN, .rst=&RCC->APB1PRST, .bit=bit};
    }
}

// Return the frequency of the given peripheral clock
uint32_t
get_pclock_frequency(uint32_t periph_base)
{
    if (periph_base >= APB2PERIPH_BASE)
        return FREQ_APB2;
    return FREQ_APB1;
}

// Enable a GPIO peripheral clock
void
gpio_clock_enable(GPIO_TypeDef *regs)
{
    uint32_t rcc_pos = ((uint32_t)regs - APB2PERIPH_BASE) / 0x400;
    RCC->APB2PCLKEN |= 1 << rcc_pos;
    RCC->APB2PCLKEN;
}

static void
clock_setup(void)
{
    // Ensure HSI is enabled (required for Flash operations) and selected before configuring PLL
    RCC->CTRL |= RCC_CTRL_HSIEN;
    while (!(RCC->CTRL & RCC_CTRL_HSIRDF))
        ;
    RCC->CFG &= ~RCC_CFG_SCLKSW;
    while ((RCC->CFG & RCC_CFG_SCLKSTS) != RCC_CFG_SCLKSTS_HSI)
        ;
    RCC->CTRL &= ~RCC_CTRL_PLLEN;

    uint32_t cfg = 0;
    uint32_t mult;

    if (!CONFIG_STM32_CLOCK_REF_INTERNAL) {
        // Configure PLL from external crystal (HSE)
        RCC->CTRL |= RCC_CTRL_HSEEN;
        while (!(RCC->CTRL & RCC_CTRL_HSERDF))
            ;
        uint32_t pll_in = CONFIG_CLOCK_REF_FREQ;
        cfg |= RCC_CFG_PLLSRC_HSE;
        if ((CONFIG_CLOCK_FREQ % pll_in) != 0) {
            cfg |= RCC_CFG_PLLHSEPRES_HSE_DIV2;
            pll_in /= 2;
        }
        mult = CONFIG_CLOCK_FREQ / pll_in;
    } else {
        // Configure PLL from internal 8MHz HSI divided by 2 (4MHz)
        cfg |= RCC_CFG_PLLSRC_HSI_DIV2;
        mult = CONFIG_CLOCK_FREQ / (HSI_VALUE / 2);
    }

    // Encode 5-bit PLLMULFCT (same logic as SetSysClock in system_n32g45x.c)
    if (mult <= 16)
        cfg |= (mult - 2) << 18;
    else
        cfg |= ((mult - 17) << 18) | RCC_CFG_PLLMULFCT_4;

    // AHB = SYSCLK, APB2 = HCLK/2 (max 72MHz), APB1 = HCLK/4 (max 36MHz)
    cfg |= RCC_CFG_AHBPRES_DIV1 | RCC_CFG_APB2PRES_DIV2 | RCC_CFG_APB1PRES_DIV4;

    // Configure 48MHz USB clock prescaler
    if (CONFIG_USB) {
        if (CONFIG_CLOCK_FREQ == 144000000)
            cfg |= RCC_CFG_USBPRES_PLLDIV3;
        else if (CONFIG_CLOCK_FREQ == 96000000)
            cfg |= RCC_CFG_USBPRES_PLLDIV2;
        else if (CONFIG_CLOCK_FREQ == 72000000)
            cfg |= RCC_CFG_USBPRES_PLLDIV1_5;
        else if (CONFIG_CLOCK_FREQ == 48000000)
            cfg |= RCC_CFG_USBPRES_PLLDIV1;
    }

    // Set Flash latency (0..4 wait states per 32MHz step) + Prefetch + iCache
    uint32_t latency = (CONFIG_CLOCK_FREQ - 1) / 32000000;
    FLASH->AC = latency | FLASH_AC_PRFTBFE | FLASH_AC_ICAHEN;

    RCC->CFG = cfg;
    RCC->CTRL |= RCC_CTRL_PLLEN;

    // Wait for PLL lock
    while (!(RCC->CTRL & RCC_CTRL_PLLRDF))
        ;

    // Switch system clock to PLL
    RCC->CFG = cfg | RCC_CFG_SCLKSW_PLL;
    while ((RCC->CFG & RCC_CFG_SCLKSTS) != RCC_CFG_SCLKSTS_PLL)
        ;
}

/****************************************************************
 * GPIO setup
 ****************************************************************/

static void
n32g45x_alternative_remap(uint32_t rmp_mask, uint32_t rmp_value)
{
    static uint32_t rmp_cfg = 0;
    rmp_cfg &= ~rmp_mask;
    rmp_cfg |= rmp_value;
    AFIO->RMP_CFG = rmp_cfg;
}

#define N32_OSPEED 0x1 // 10MHz max output speed

void
gpio_peripheral(uint32_t gpio, uint32_t mode, int pullup)
{
    GPIO_TypeDef *regs = digital_regs[GPIO2PORT(gpio)];
    gpio_clock_enable(regs);

    uint32_t pos = gpio % 16, shift = (pos % 8) * 4, msk = 0xf << shift, cfg;
    if (mode == GPIO_INPUT) {
        cfg = pullup ? 0x8 : 0x4;
    } else if (mode == GPIO_OUTPUT) {
        cfg = N32_OSPEED;
    } else if (mode == (GPIO_OUTPUT | GPIO_OPEN_DRAIN)) {
        cfg = 0x4 | N32_OSPEED;
    } else if (mode == GPIO_ANALOG) {
        cfg = 0x0;
    } else {
        if (mode & GPIO_OPEN_DRAIN)
            cfg = 0xc | N32_OSPEED;
        else if (pullup > 0)
            cfg = 0x8;
        else
            cfg = 0x8 | N32_OSPEED;
    }
    if (pos & 0x8)
        regs->PH_CFG = (regs->PH_CFG & ~msk) | (cfg << shift);
    else
        regs->PL_CFG = (regs->PL_CFG & ~msk) | (cfg << shift);

    if (pullup > 0)
        regs->PBSC = 1 << pos;
    else if (pullup < 0)
        regs->PBSC = 1 << (pos + 16);

    if (gpio == GPIO('A', 13) || gpio == GPIO('A', 14))
        n32g45x_alternative_remap(AFIO_RMP_CFG_SW_JTAG_CFG,
                                  AFIO_RMP_CFG_SW_JTAG_CFG_DISABLE);

    uint32_t func = (mode >> 4) & 0xf;
    if (func == 7) {
        // USART1/2/3 remapping
        if (gpio == GPIO('B', 6) || gpio == GPIO('B', 7))
            n32g45x_alternative_remap(AFIO_RMP_CFG_USART1_RMP,
                                      AFIO_RMP_CFG_USART1_RMP);
        else if (gpio == GPIO('D', 5) || gpio == GPIO('D', 6))
            n32g45x_alternative_remap(AFIO_RMP_CFG_USART2_RMP_0,
                                      AFIO_RMP_CFG_USART2_RMP_0);
        else if (gpio == GPIO('D', 8) || gpio == GPIO('D', 9))
            n32g45x_alternative_remap(AFIO_RMP_CFG_USART3_RMP,
                                      AFIO_RMP_CFG_USART3_RMP_ALL);
    } else if (func == 9) {
        // CAN1 remapping
        if (gpio == GPIO('B', 8) || gpio == GPIO('B', 9))
            n32g45x_alternative_remap(AFIO_RMP_CFG_CAN1_RMP,
                                      AFIO_RMP_CFG_CAN1_RMP_RMP2);
        else if (gpio == GPIO('D', 0) || gpio == GPIO('D', 1))
            n32g45x_alternative_remap(AFIO_RMP_CFG_CAN1_RMP,
                                      AFIO_RMP_CFG_CAN1_RMP_RMP3);
    }
}

/****************************************************************
 * Bootloader & Startup
 ****************************************************************/

void
bootloader_request(void)
{
    try_request_canboot();
    dfu_reboot();
}

void
armcm_main(void)
{
    dfu_reboot_check();

    SystemInit();
    SCB->VTOR = (uint32_t)VectorTable;

    // Reset peripheral clocks (keep SRAMEN and FLITFEN enabled)
    RCC->AHBPCLKEN = RCC_AHBPCLKEN_SRAMEN | RCC_AHBPCLKEN_FLITFEN;
    RCC->APB1PCLKEN = 0;
    RCC->APB2PCLKEN = 0;

    clock_setup();

    // Disable JTAG to free PA15, PB3, PB4 while keeping SWD enabled
    enable_pclock(AFIO_BASE);
    n32g45x_alternative_remap(AFIO_RMP_CFG_SW_JTAG_CFG,
                              AFIO_RMP_CFG_SW_JTAG_CFG_SW_ENABLE);

    sched_main();
}
