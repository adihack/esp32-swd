/* SPDX-License-Identifier: MIT */
#include "soc/gpio_reg.h"
#include "soc/soc.h"
#include "board_r5_pins.h"

/* Referenced by the bootloader build so the linker keeps this file (the hooks are weak symbols). */
void bootloader_hooks_include(void)
{
}

/* Runs before any bootloader initialisation: only plain register writes are allowed here. Clearing output latches
 * is harmless on pins that are inputs; on pins still configured as outputs (CPU-only reset) it drives them low. */
void bootloader_before_init(void)
{
    REG_WRITE(GPIO_OUT_W1TC_REG, R5_SIG_LO);
    REG_WRITE(GPIO_OUT1_W1TC_REG, R5_SIG_HI);
    REG_WRITE(GPIO_OUT_W1TC_REG, R5_NRST_LO);
    REG_WRITE(GPIO_OUT1_W1TC_REG, R5_NRST_HI);
    REG_WRITE(GPIO_OUT_W1TC_REG, R5_VTREF_LO);
    REG_WRITE(GPIO_OUT1_W1TC_REG, R5_VTREF_HI);
}
