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
#include "canboot.h" // REQUEST_CANBOOT, get_bootup_code

/****************************************************************
 * Compile-time clock validation
 ****************************************************************/

#if !CONFIG_STM32_CLOCK_REF_INTERNAL \
    && (2 * CONFIG_CLOCK_FREQ) % CONFIG_CLOCK_REF_FREQ
  #error "Unable to generate the requested clock rate from this crystal"
#endif

#if CONFIG_USB && CONFIG_CLOCK_FREQ != 144000000 \
    && CONFIG_CLOCK_FREQ != 96000000 \
    && CONFIG_CLOCK_FREQ != 72000000 \
    && CONFIG_CLOCK_FREQ != 48000000
  #error "Unable to generate a 48Mhz usb clock at this system clock rate"
#endif


/****************************************************************
 * BKP-register boot-code storage
 *
 * NVIC_SystemReset() is a "local" (software) reset that preserves SRAM, but
 * Cortex-M write buffering means a store to Normal memory may not reach SRAM
 * before the reset if no DSB is executed. Klipper (compiled for stm32f103xe)
 * calls try_request_canboot() from its own armcm_reset.c which may lack the
 * DSB barrier, causing the REQUEST_CANBOOT value to be lost.
 *
 * The N32G45X Backup Domain registers (BKP->DAT1..DAT4) are Device-type
 * memory; writes to them are never buffered and are always committed before
 * the next instruction, so they survive a system reset without any ordering
 * hazard.
 ****************************************************************/

// Enable write access to the backup domain.
static void
bkp_enable_write(void)
{
    // Enable PWR and BKP peripheral clocks
    RCC->APB1PCLKEN |= RCC_APB1PCLKEN_PWREN | RCC_APB1PCLKEN_BKPEN;
    // Set DBP (Disable Backup write Protection) bit
    PWR->CTRL |= PWR_CTRL_DBKP;
}

// Disable write access (restore write protection).
static void
bkp_disable_write(void)
{
    PWR->CTRL &= ~PWR_CTRL_DBKP;
}

// Write a 64-bit value into BKP registers DAT1..DAT4 (caller must enable write)
static void
bkp_write_u64(uint64_t code)
{
    BKP->DAT1 = (uint16_t)(code        & 0xFFFF);
    BKP->DAT2 = (uint16_t)((code >> 16) & 0xFFFF);
    BKP->DAT3 = (uint16_t)((code >> 32) & 0xFFFF);
    BKP->DAT4 = (uint16_t)((code >> 48) & 0xFFFF);
}

// Forward declaration – _stack_end is the linker symbol used by the generic
// armcm_canboot.c implementation; we reuse the same address as a RAM fallback
// for Klipper (stm32f103xe) compatibility.
extern uint32_t _stack_end;

// Read the 64-bit boot code.
//
// Primary source: BKP registers DAT1-DAT4 (device memory, no write-buffering).
// Fallback source: RAM word at _stack_end (written by Klipper's own copy of
//   try_request_canboot() which writes to bl_vectors[0] == &_stack_end).
//   If the RAM value is non-zero we migrate it into BKP and clear RAM so that
//   stale RAM words from previous boots are never misread.
uint64_t
get_bootup_code(void)
{
    // Temporarily enable BKP clock (DBP is only needed for writes)
    uint32_t saved_apb1 = RCC->APB1PCLKEN;
    RCC->APB1PCLKEN |= RCC_APB1PCLKEN_BKPEN;

    uint64_t bkp_code = ((uint64_t)BKP->DAT1)
                       | ((uint64_t)BKP->DAT2 << 16)
                       | ((uint64_t)BKP->DAT3 << 32)
                       | ((uint64_t)BKP->DAT4 << 48);

    // Also read the RAM location that Klipper's try_request_canboot() writes.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
    volatile uint64_t *ram_sig = (volatile uint64_t *)&_stack_end;
    uint64_t ram_code = *ram_sig;
#pragma GCC diagnostic pop

    uint64_t code = bkp_code;

    if (ram_code == REQUEST_CANBOOT || (bkp_code == 0 && ram_code != 0)) {
        // Klipper explicitly requested the bootloader via RAM, or BKP is empty
        // while RAM holds a boot signature. Migrate it into BKP so subsequent
        // reads within bootentry_check() see it, and clear RAM.
        bkp_enable_write();
        bkp_write_u64(ram_code);
        bkp_disable_write();
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
        *ram_sig = 0;
#pragma GCC diagnostic pop
        __DSB();
        code = ram_code;
    }

    RCC->APB1PCLKEN = saved_apb1;
    return code;
}

// Write the 64-bit boot code to BKP registers DAT1..DAT4.
// Also clear the RAM fallback location so a stale RAM value is never
// misread as a boot request on the next call to get_bootup_code().
void
set_bootup_code(uint64_t code)
{
    bkp_enable_write();
    bkp_write_u64(code);
    bkp_disable_write();

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
    volatile uint64_t *ram_sig = (volatile uint64_t *)&_stack_end;
    *ram_sig = 0;
#pragma GCC diagnostic pop
    __DSB();
}


/****************************************************************
 * Clock setup
 ****************************************************************/

// The N32G45x adds a third PWR control register at offset 0x0c holding the
// "ex mode" enable bit required to clock ADC, SDIO, QSPI, OPAMP, COMP, & CAN2.
#define N32_PWR_CTRL3 (*(volatile uint32_t *)(PWR_BASE + 0x0c))

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
    if (periph_base >= APB2PERIPH_BASE) {
        // APB2 (PCLK2) is rated up to 72MHz max
        if (CONFIG_CLOCK_FREQ > 72000000)
            return CONFIG_CLOCK_FREQ / 2;
        return CONFIG_CLOCK_FREQ;
    }
    // APB1 (PCLK1) is rated up to 36MHz max
    if (CONFIG_CLOCK_FREQ > 72000000)
        return CONFIG_CLOCK_FREQ / 4;
    if (CONFIG_CLOCK_FREQ > 36000000)
        return CONFIG_CLOCK_FREQ / 2;
    return CONFIG_CLOCK_FREQ;
}

// Enable a GPIO peripheral clock
void
gpio_clock_enable(GPIO_TypeDef *regs)
{
    uint32_t rcc_pos = ((uint32_t)regs - APB2PERIPH_BASE) / 0x400;
    RCC->APB2PCLKEN |= 1 << rcc_pos;
    (void)RCC->APB2PCLKEN;
}

static void
clock_setup(void)
{
    // Enable N32G45x "ex mode" in PWR_CTRL3 so ADC, SDIO, QSPI, OPAMP, COMP,
    // and CAN2 RCC enable bits are ungated even if SystemInit() did not set it.
    RCC->APB1PCLKEN |= RCC_APB1PCLKEN_PWREN;
    N32_PWR_CTRL3 |= 1;
    RCC->APB1PCLKEN &= ~RCC_APB1PCLKEN_PWREN;

    // Ensure HSI is enabled (required for Flash operations) and selected
    // as the active system clock before disabling and reconfiguring the PLL.
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

    // Encode 5-bit PLLMULFCT (multipliers 2..16 vs 17..32)
    if (mult <= 16)
        cfg |= (mult - 2) << 18;
    else
        cfg |= ((mult - 17) << 18) | RCC_CFG_PLLMULFCT_4;

    // Dynamically scale APB2 (max 72MHz) and APB1 (max 36MHz) prescalers
    cfg |= RCC_CFG_AHBPRES_DIV1;
    if (CONFIG_CLOCK_FREQ > 72000000)
        cfg |= RCC_CFG_APB2PRES_DIV2 | RCC_CFG_APB1PRES_DIV4;
    else if (CONFIG_CLOCK_FREQ > 36000000)
        cfg |= RCC_CFG_APB2PRES_DIV1 | RCC_CFG_APB1PRES_DIV2;
    else
        cfg |= RCC_CFG_APB2PRES_DIV1 | RCC_CFG_APB1PRES_DIV1;

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

// Bitmasks for standard AFIO->RMP_CFG remappings shared with STM32F1 MAPR
#define N32_RMP_SPI1_MSK        (1U << 0)
#define N32_RMP_I2C1_MSK        (1U << 1)
#define N32_RMP_TIM2_MSK        (3U << 8)
#define N32_RMP_TIM2_PARTIAL1   (1U << 8)
#define N32_RMP_TIM2_PARTIAL2   (2U << 8)
#define N32_RMP_TIM3_MSK        (3U << 10)
#define N32_RMP_TIM3_PARTIAL    (2U << 10)
#define N32_RMP_TIM3_FULL       (3U << 10)
#define N32_RMP_TIM4_MSK        (1U << 12)

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
    if (func == 1) {
        // TIM2 remapping
        if (gpio == GPIO('A', 15) || gpio == GPIO('B', 3))
            n32g45x_alternative_remap(N32_RMP_TIM2_MSK, N32_RMP_TIM2_PARTIAL1);
        else if (gpio == GPIO('B', 10) || gpio == GPIO('B', 11))
            n32g45x_alternative_remap(N32_RMP_TIM2_MSK, N32_RMP_TIM2_PARTIAL2);
    } else if (func == 2) {
        // TIM3 and TIM4 remapping
        if (gpio == GPIO('B', 4) || gpio == GPIO('B', 5))
            n32g45x_alternative_remap(N32_RMP_TIM3_MSK, N32_RMP_TIM3_PARTIAL);
        else if (gpio == GPIO('C', 6) || gpio == GPIO('C', 7)
                 || gpio == GPIO('C', 8) || gpio == GPIO('C', 9))
            n32g45x_alternative_remap(N32_RMP_TIM3_MSK, N32_RMP_TIM3_FULL);
        else if (gpio == GPIO('D', 12) || gpio == GPIO('D', 13)
                 || gpio == GPIO('D', 14) || gpio == GPIO('D', 15))
            n32g45x_alternative_remap(N32_RMP_TIM4_MSK, N32_RMP_TIM4_MSK);
    } else if (func == 4) {
        // I2C1 remapping
        if (gpio == GPIO('B', 8) || gpio == GPIO('B', 9))
            n32g45x_alternative_remap(N32_RMP_I2C1_MSK, N32_RMP_I2C1_MSK);
    } else if (func == 5) {
        // SPI1 remapping
        if (gpio == GPIO('B', 3) || gpio == GPIO('B', 4)
            || gpio == GPIO('B', 5))
            n32g45x_alternative_remap(N32_RMP_SPI1_MSK, N32_RMP_SPI1_MSK);
    } else if (func == 7) {
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
