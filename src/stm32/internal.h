#ifndef __STM32_INTERNAL_H
#define __STM32_INTERNAL_H
// Local definitions for STM32 code

#include "autoconf.h" // CONFIG_MACH_STM32F1

#if CONFIG_MACH_STM32F0
#include "stm32f0xx.h"
#elif CONFIG_MACH_STM32F1
#include "stm32f1xx.h"
#elif CONFIG_MACH_STM32F2
#include "stm32f2xx.h"
#elif CONFIG_MACH_STM32F4
#include "stm32f4xx.h"
#elif CONFIG_MACH_STM32G0
#include "stm32g0xx.h"
#elif CONFIG_MACH_STM32G4
#include "stm32g4xx.h"
#elif CONFIG_MACH_STM32H7
#include "stm32h7xx.h"
#elif CONFIG_MACH_STM32L4
#include "stm32l4xx.h"
#elif CONFIG_MACH_N32G45X
#include "n32g45x.h"
#endif


// 1. Map Nation's *_Module struct types to STM32 *_TypeDef names
typedef GPIO_Module  GPIO_TypeDef;
typedef USART_Module USART_TypeDef;
typedef CAN_Module   CAN_TypeDef;
typedef USB_Module   USB_TypeDef;
typedef RCC_Module   RCC_TypeDef;
typedef FLASH_Module FLASH_TypeDef;
typedef AFIO_Module  AFIO_TypeDef;
typedef BKP_Module   BKP_TypeDef;
typedef PWR_Module   PWR_TypeDef;

// 2. Flash register & bit definitions (User Manual Section 3.2.5)
#ifndef FLASH_AC_LATENCY
#define FLASH_AC_LATENCY  ((uint32_t)0x00000007)
#define FLASH_AC_PRFTBFE  ((uint32_t)0x00000010)
#define FLASH_AC_PRFTBFEN FLASH_AC_PRFTBFE
#define FLASH_AC_PRFTBFS  ((uint32_t)0x00000020)
#define FLASH_AC_ICAHRST  ((uint32_t)0x00000040)
#define FLASH_AC_ICAHEN   ((uint32_t)0x00000080)
#endif
#ifndef FLASH_STS_BUSY
#define FLASH_STS_BUSY    ((uint32_t)0x00000001)
#define FLASH_STS_PGERR   ((uint32_t)0x00000004)
#define FLASH_STS_PVERR   ((uint32_t)0x00000008)
#define FLASH_STS_WRPERR  ((uint32_t)0x00000010)
#define FLASH_STS_EOP     ((uint32_t)0x00000020)
#define FLASH_STS_EVERR   ((uint32_t)0x00000040)
#define FLASH_STS_ECCERR  ((uint32_t)0x00000080)
#endif
#ifndef FLASH_CTRL_PG
#define FLASH_CTRL_PG     ((uint32_t)0x00000001)
#define FLASH_CTRL_PER    ((uint32_t)0x00000002)
#define FLASH_CTRL_MER    ((uint32_t)0x00000004)
#define FLASH_CTRL_OPTPG  ((uint32_t)0x00000010)
#define FLASH_CTRL_OPTER  ((uint32_t)0x00000020)
#define FLASH_CTRL_START  ((uint32_t)0x00000040)
#define FLASH_CTRL_LOCK   ((uint32_t)0x00000080)
#endif

// 2b. N32G45X FLASH struct member aliases: map STM32 names -> N32 names
// The N32G45X FLASH_Module uses AC/KEY/STS/CTRL/ADD instead of
// ACR/KEYR/SR/CR/AR used by STM32 parts.  These aliases let flash.c
// use the canonical STM32 names on both families.
#if CONFIG_MACH_N32G45X
#define SR   STS
#define CR   CTRL
#define KEYR KEY
#define AR   ADD
// Bit-level aliases: STM32 -> N32G45X
#define FLASH_SR_BSY    FLASH_STS_BUSY
#define FLASH_CR_LOCK   FLASH_CTRL_LOCK
#define FLASH_CR_PER    FLASH_CTRL_PER
#define FLASH_CR_PG     FLASH_CTRL_PG
#define FLASH_CR_STRT   FLASH_CTRL_START
// FLASHSIZE_BASE does not exist on N32G45X; flash.c uses a dedicated
// CONFIG_MACH_N32G45X branch that returns the fixed page size instead.
// A dummy value is required so the compiler can parse the dead STM32F1/F0
// branches in flash.c (those if-branches are never reached at runtime).
#define FLASHSIZE_BASE 0
// FLASH_AC_PRFTBFE is an alias for FLASH_AC_PRFTBFEN (the N32 header only
// defines the version with the trailing N, but n32g45x.c uses the shorter form).
#ifndef FLASH_AC_PRFTBFE
#define FLASH_AC_PRFTBFE FLASH_AC_PRFTBFEN
#endif
#endif

// 3. CAN additional aliases for can.c (N32G45X)
#if CONFIG_MACH_N32G45X
// --- Type aliases: STM32 names -> N32G45X names ---
typedef CAN_TxMailBox_Param    CAN_TxMailBox_TypeDef;
typedef CAN_FIFOMailBox_Param  CAN_FIFOMailBox_TypeDef;
typedef CAN_Module             CAN_TypeDef;

// --- MCU-selection defines used in can.c ---
#define SOC_CAN         CAN1
#define FILTER_CAN      CAN1
// CAN1 TX shares the USB_HP vector on N32G45X
#define CAN_RX0_IRQn    USB_LP_CAN1_RX0_IRQn
#define CAN_RX1_IRQn    CAN1_RX1_IRQn
#define CAN_TX_IRQn     USB_HP_CAN1_TX_IRQn
#define CAN_SCE_IRQn    CAN1_SCE_IRQn
// AF9 (same as STM32F1) for CAN pin remapping in gpio_peripheral()
#define CAN_FUNCTION    GPIO_FUNCTION(9)

// --- CAN_BTR/BTIM bit-position aliases ---
#define CAN_BTR_BRP_Pos   0

// --- CAN mailbox register aliases: STM32 -> N32G45X ---
// TSR -> TSTS (already done in section 6 of internal.h for top-level regs)
// Bit-level: TSR bits
#define CAN_TSR_RQCP0   CAN_TSTS_RQCPM0
#define CAN_TSR_RQCP1   CAN_TSTS_RQCPM1
#define CAN_TSR_RQCP2   CAN_TSTS_RQCPM2
// IER bits
#define CAN_IER_TMEIE   CAN_INTE_TMEITE
#define CAN_IER_FMPIE0  CAN_INTE_FMPITE0
// RF0R bits
#define CAN_RF0R_FMP0   CAN_RFF0_FFMP0
#define CAN_RF0R_RFOM0  CAN_RFF0_RFFOM0
// Tx mailbox identifier register bits
#define CAN_TI0R_STID   CAN_TMI0_STDID
#define CAN_TI0R_STID_Pos 21
#define CAN_TI0R_EXID_Pos 3
// RDT0R / RDTR DLC
#define CAN_RDT0R_DLC   CAN_RMDT0_DLC
// Filter Management Register
#define CAN_FMR_CAN2SB  ((uint32_t)0x00003F00)  /* CAN2 start bank bits [13:8] */
#endif // CONFIG_MACH_N32G45X

// 3. GPIO register aliases for stm32/gpio.c
#define CRL  PL_CFG
#define CRH  PH_CFG
#define IDR  PID
#define ODR  POD
#define BSRR PBSC
#define BRR  PBC

// 4. PWR & BKP aliases for stm32/dfu_reboot.c
#define DR1  DAT1
#define DR4  DAT4
#define DR10 DAT10
#define PWR_CR_DBP       PWR_CTRL_DBKP
#define RCC_APB1ENR_PWREN RCC_APB1PCLKEN_PWREN
#define RCC_APB1ENR_BKPEN RCC_APB1PCLKEN_BKPEN

// 5. USART register & bit aliases for stm32/serial.c (User Manual Section 23.7)
#define SR  STS
#define DR  DAT
#define CR1 CTRL1
#define CR2 CTRL2
#define CR3 CTRL3
#ifndef USART_SR_RXNE
#define USART_SR_PE      ((uint16_t)0x0001)
#define USART_SR_FE      ((uint16_t)0x0002)
#define USART_SR_NE      ((uint16_t)0x0004)
#define USART_SR_ORE     ((uint16_t)0x0008)
#define USART_SR_IDLE    ((uint16_t)0x0010)
#define USART_SR_RXNE    ((uint16_t)0x0020)
#define USART_SR_TC      ((uint16_t)0x0040)
#define USART_SR_TXE     ((uint16_t)0x0080)
#define USART_CR1_RE     ((uint16_t)0x0004)
#define USART_CR1_TE     ((uint16_t)0x0008)
#define USART_CR1_RXNEIE ((uint16_t)0x0020)
#define USART_CR1_TCIE   ((uint16_t)0x0040)
#define USART_CR1_TXEIE  ((uint16_t)0x0080)
#define USART_CR1_UE     ((uint16_t)0x2000)
#endif

// 6. bxCAN register & bit aliases for stm32/can.c
#define MCR   MCTRL
#define MSR   MSTS
#define TSR   TSTS
#define RF0R  RFF0
#define RF1R  RFF1
#define IER   INTE
#define ESR   ESTS
#define BTR   BTIM
#define FMR   FMC
#define FM1R  FM1
#define FS1R  FS1
#define FFA1R FFA1
#define FA1R  FA1
#define TIR   TMI
#define TDTR  TMDT
#define TDLR  TMDL
#define TDHR  TMDH
#define RIR   RMI
#define RDTR  RMDT
#define RDLR  RMDL
#define RDHR  RMDH
#define CAN_MCR_INRQ     CAN_MCTRL_INIRQ
#define CAN_MCR_NART     CAN_MCTRL_NART
#define CAN_MCR_ABOM     CAN_MCTRL_ABOM
#define CAN_MCR_TXFP     CAN_MCTRL_TXFP
#define CAN_MSR_INAK     CAN_MSTS_INIAK
#define CAN_TSR_TME0     CAN_TSTS_TMEM0
#define CAN_TSR_TME1     CAN_TSTS_TMEM1
#define CAN_TSR_TME2     CAN_TSTS_TMEM2
#define CAN_RF0R_FMP0    CAN_RFF0_FFMP0
#define CAN_RF0R_RFOM0   CAN_RFF0_RFFOM0
#define CAN_IER_FMPIE0   CAN_INTE_FMPITE0
#define CAN_IER_TMEIE    CAN_INTE_TMEITE
#define CAN_TI0R_TXRQ    CAN_TMI0_TXRQ
#define CAN_TI0R_IDE     CAN_TMI0_IDE
#define CAN_TI0R_RTR     CAN_TMI0_RTRQ
#define CAN_TI0R_STID_Pos 21
#define CAN_TI0R_EXID_Pos 3
#define CAN_RI0R_IDE     CAN_RMI0_IDE
#define CAN_RI0R_RTR     CAN_RMI0_RTRQ
#define CAN_RI0R_STID_Pos 21
#define CAN_RI0R_EXID_Pos 3
#define CAN_RDTR_DLC     CAN_RMDT0_DLC
#define CAN_FMR_FINIT    CAN_FMC_FINITM
#define CAN_BTR_SJW_Pos  24
#define CAN_BTR_TS2_Pos  20
#define CAN_BTR_TS1_Pos  16

// 7. USB_FS_Device register & bit aliases for stm32/usbfs.c
#define USB_PMAADDR      USB_CAN1_SRAM_BASE
#define EP0R             EP0
#define CNTR             CTRL
#define ISTR             STS
#define FNR              FN
#define DADDR            ADDR
#define BTABLE           BUFTAB
#define USB_CNTR_FRES    USB_CTRL_FRST
#define USB_CNTR_PDWN    USB_CTRL_PD
#define USB_CNTR_CTRM    USB_CTRL_CTRSM
#define USB_CNTR_RESETM  USB_CTRL_RSTM
#define USB_CNTR_SUSPM   USB_CTRL_SUSPDM
#define USB_CNTR_WKUPM   USB_CTRL_WKUPM
#define USB_ISTR_CTR     USB_STS_CTRS
#define USB_ISTR_RESET   USB_STS_RST
#define USB_ISTR_SUSP    USB_STS_SUSPD
#define USB_DADDR_EF     USB_ADDR_EFUC
#define USB_DADDR_ADD    USB_ADDR_ADDR
#define USB_EP_CTR_RX    USB_EP0_CTRS_RX
#define USB_EP_DTOG_RX   USB_EP0_DATTOG_RX
#define USB_EPRX_STAT    USB_EP0_STS_RX
#define USB_EP_SETUP     USB_EP0_SETUP
#define USB_EP_T_FIELD   USB_EP0_EP_TYPE
#define USB_EP_KIND      USB_EP0_EP_KIND
#define USB_EP_CTR_TX    USB_EP0_CTRS_TX
#define USB_EP_DTOG_TX   USB_EP0_DATTOG_TX
#define USB_EPTX_STAT    USB_EP0_STS_TX
#define USB_EPADDR_FIELD USB_EP0_EPADDR
#define USB_EP_CONTROL   ((uint16_t)0x0200)
#define USB_EP_BULK      ((uint16_t)0x0000)
#define USB_EP_INTERRUPT ((uint16_t)0x0600)
#define USB_EP_RX_DIS    ((uint16_t)0x0000)
#define USB_EP_RX_STALL  ((uint16_t)0x1000)
#define USB_EP_RX_NAK    ((uint16_t)0x2000)
#define USB_EP_RX_VALID  ((uint16_t)0x3000)
#define USB_EP_TX_DIS    ((uint16_t)0x0000)
#define USB_EP_TX_STALL  ((uint16_t)0x0010)
#define USB_EP_TX_NAK    ((uint16_t)0x0020)
#define USB_EP_TX_VALID  ((uint16_t)0x0030)
// gpio.c
extern GPIO_TypeDef * const digital_regs[];
#define GPIO(PORT, NUM) (((PORT)-'A') * 16 + (NUM))
#define GPIO2PORT(PIN) ((PIN) / 16)
#define GPIO2BIT(PIN) (1<<((PIN) % 16))

// gpioperiph.c
#define GPIO_INPUT 0
#define GPIO_OUTPUT 1
#define GPIO_OPEN_DRAIN 0x100
#define GPIO_FUNCTION(fn) (2 | ((fn) << 4))
#define GPIO_ANALOG 3
void gpio_peripheral(uint32_t gpio, uint32_t mode, int pullup);

// clockline.c
void enable_pclock(uint32_t periph_base);
int is_enabled_pclock(uint32_t periph_base);

// dfu_reboot.c
void dfu_reboot(void);
void dfu_reboot_check(void);

// stm32??.c
struct cline { volatile uint32_t *en, *rst; uint32_t bit; };
struct cline lookup_clock_line(uint32_t periph_base);
uint32_t get_pclock_frequency(uint32_t periph_base);
void gpio_clock_enable(GPIO_TypeDef *regs);

#endif // internal.h
