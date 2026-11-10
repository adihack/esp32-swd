/* SPDX-License-Identifier: MIT */
/* esp-swd target module: STM32U5 (STM32U575/585, DEV_ID 0x482; STM32U59x/5Ax, DEV_ID 0x481).
 * Registers and bits from ST's CMSIS device header stm32u585xx.h and the U5 HAL (stm32u5xx_hal_flash.h). */
#ifndef ESW_TARGETS_STM32U5_H
#define ESW_TARGETS_STM32U5_H
#include "esw/target.h"

#define ESW_U5_DBGMCU_IDCODE  0xE0044000u
#define ESW_U5_DBGMCU_CR      0xE0044004u
#define ESW_U5_DBGMCU_APB1FZR1 0xE0044008u
#define ESW_U5_FZR1_WWDG      (1u << 11)
#define ESW_U5_FZR1_IWDG      (1u << 12)

#define ESW_U5_FLASH_NS       0x40022000u   /* FLASH registers, non-secure alias */
#define ESW_U5_FLASH_NSKEYR   0x08u
#define ESW_U5_FLASH_OPTKEYR  0x10u
#define ESW_U5_FLASH_NSSR     0x20u
#define ESW_U5_FLASH_NSCR     0x28u
#define ESW_U5_FLASH_OPTR     0x40u

#define ESW_U5_SR_EOP         (1u << 0)
#define ESW_U5_SR_OPERR       (1u << 1)
#define ESW_U5_SR_PROGERR     (1u << 3)
#define ESW_U5_SR_WRPERR      (1u << 4)
#define ESW_U5_SR_PGAERR      (1u << 5)
#define ESW_U5_SR_SIZERR      (1u << 6)
#define ESW_U5_SR_PGSERR      (1u << 7)
#define ESW_U5_SR_OPTWERR     (1u << 13)
#define ESW_U5_SR_BSY         (1u << 16)
#define ESW_U5_SR_WDW         (1u << 17)
#define ESW_U5_SR_OEM1LOCK    (1u << 18)
#define ESW_U5_SR_ERRORS      (ESW_U5_SR_OPERR | ESW_U5_SR_PROGERR | ESW_U5_SR_WRPERR | ESW_U5_SR_PGAERR | \
                               ESW_U5_SR_SIZERR | ESW_U5_SR_PGSERR | ESW_U5_SR_OPTWERR)

#define ESW_U5_CR_PG          (1u << 0)
#define ESW_U5_CR_PER         (1u << 1)
#define ESW_U5_CR_MER1        (1u << 2)
#define ESW_U5_CR_PNB_POS     3u
#define ESW_U5_CR_BKER        (1u << 11)
#define ESW_U5_CR_MER2        (1u << 15)
#define ESW_U5_CR_STRT        (1u << 16)
#define ESW_U5_CR_OPTSTRT     (1u << 17)
#define ESW_U5_CR_OBL_LAUNCH  (1u << 27)
#define ESW_U5_CR_OPTLOCK     (1u << 30)
#define ESW_U5_CR_LOCK        (1u << 31)

#define ESW_U5_OPTR_RDP_MASK  0xFFu
#define ESW_U5_RDP_L0         0xAAu
#define ESW_U5_RDP_L0_5       0x55u
#define ESW_U5_RDP_L2         0xCCu   /* any other value: level 1 */
#define ESW_U5_OPTR_SWAP_BANK (1u << 20)
#define ESW_U5_OPTR_DUALBANK  (1u << 21)
#define ESW_U5_OPTR_TZEN      (1u << 31)

#define ESW_U5_KEY1           0x45670123u
#define ESW_U5_KEY2           0xCDEF89ABu
#define ESW_U5_OPTKEY1        0x08192A3Bu
#define ESW_U5_OPTKEY2        0x4C5D6E7Fu

#define ESW_U5_FLASH_BASE     0x08000000u
#define ESW_U5_PAGE_SIZE      0x2000u
#define ESW_U5_FLASHSIZE      0x0BFA07A0u   /* flash size in KB (16 bits) */
#define ESW_U5_SRAM1          0x20000000u   /* SRAM1 192 KB + SRAM2 64 KB, contiguous */
#define ESW_U5_ICACHE_CR      0x40030400u   /* bit 0 EN, bit 1 CACHEINV */

extern const esw_target_driver_t esw_target_stm32u5;

#endif
