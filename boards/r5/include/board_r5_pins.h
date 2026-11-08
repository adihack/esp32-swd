/* SPDX-License-Identifier: MIT */
/* r5 pin numbers as plain macros: shared by the board layer, the panic wrapper and the 2nd-stage bootloader hook
 * (which cannot link app components). Source of truth: docs/hardware-reference.md section 2. */
#ifndef BOARD_R5_PINS_H
#define BOARD_R5_PINS_H

#define R5_A_SWDIO 4
#define R5_A_SWCLK 5
#define R5_A_SWO 6
#define R5_A_NRST_DRV 7
#define R5_A_NRST_SENSE 15
#define R5_A_UART_TX 16
#define R5_A_UART_RX 17
#define R5_A_VTREF_ADC 8
#define R5_A_EN_SWD 9
#define R5_A_EN_UART 46
#define R5_A_EN_VTREF 10

#define R5_B_SWDIO 11
#define R5_B_SWCLK 12
#define R5_B_SWO 14
#define R5_B_NRST_DRV 21
#define R5_B_NRST_SENSE 47
#define R5_B_UART_TX 48
#define R5_B_UART_RX 35
#define R5_B_VTREF_ADC 1
#define R5_B_EN_SWD 40
#define R5_B_EN_UART 41
#define R5_B_EN_VTREF 42

#define R5_LED 2

/* Output-latch masks for GPIO_OUT_W1TC (pins 0-31) and GPIO_OUT1_W1TC (pins 32-48), in isolation order:
 * signal enables first, then the reset drivers, VTref sense last. */
#define R5_BIT(p) ((p) < 32 ? (1u << (p)) : 0u)
#define R5_BIT1(p) ((p) >= 32 ? (1u << ((p) - 32)) : 0u)
#define R5_SIG_LO (R5_BIT(R5_A_EN_SWD) | R5_BIT(R5_A_EN_UART) | R5_BIT(R5_B_EN_SWD) | R5_BIT(R5_B_EN_UART))
#define R5_SIG_HI (R5_BIT1(R5_A_EN_SWD) | R5_BIT1(R5_A_EN_UART) | R5_BIT1(R5_B_EN_SWD) | R5_BIT1(R5_B_EN_UART))
#define R5_NRST_LO (R5_BIT(R5_A_NRST_DRV) | R5_BIT(R5_B_NRST_DRV))
#define R5_NRST_HI (R5_BIT1(R5_A_NRST_DRV) | R5_BIT1(R5_B_NRST_DRV))
#define R5_VTREF_LO (R5_BIT(R5_A_EN_VTREF) | R5_BIT(R5_B_EN_VTREF))
#define R5_VTREF_HI (R5_BIT1(R5_A_EN_VTREF) | R5_BIT1(R5_B_EN_VTREF))

#endif
