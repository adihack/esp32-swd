/* SPDX-License-Identifier: MIT */
/* esp-swd: Cortex-M (ARMv6-M/v7-M/v8-M) implementation of the core-control interface. */
#ifndef ESW_CORTEXM_H
#define ESW_CORTEXM_H
#include "esw/cpu.h"

#define ESW_CM_CPUID   0xE000ED00u
#define ESW_CM_AIRCR   0xE000ED0Cu
#define ESW_CM_DFSR    0xE000ED30u
#define ESW_CM_DHCSR   0xE000EDF0u
#define ESW_CM_DCRSR   0xE000EDF4u
#define ESW_CM_DCRDR   0xE000EDF8u
#define ESW_CM_DEMCR   0xE000EDFCu

#define ESW_DHCSR_DBGKEY     0xA05F0000u
#define ESW_DHCSR_C_DEBUGEN  (1u << 0)
#define ESW_DHCSR_C_HALT     (1u << 1)
#define ESW_DHCSR_S_REGRDY   (1u << 16)
#define ESW_DHCSR_S_HALT     (1u << 17)
#define ESW_DHCSR_S_LOCKUP   (1u << 19)
#define ESW_DHCSR_S_RESET_ST (1u << 25)
#define ESW_DCRSR_REGWNR     (1u << 16)
#define ESW_AIRCR_SYSRESETREQ 0x05FA0004u
#define ESW_DEMCR_VC_CORERESET (1u << 0)
#define ESW_DEMCR_VC_HARDERR   (1u << 10)
#define ESW_XPSR_T           0x01000000u

void esw_cortexm_init(esw_cpu_t *c, esw_memap_t *mem);

/* Raw Cortex-M register access by DCRSR number (0-12 R0-R12, 13 SP, 14 LR, 15 PC, 16 xPSR). */
esw_err_t esw_cortexm_reg_read(esw_cpu_t *c, uint32_t regsel, uint32_t *val);
esw_err_t esw_cortexm_reg_write(esw_cpu_t *c, uint32_t regsel, uint32_t val);

#endif
