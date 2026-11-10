/* SPDX-License-Identifier: MIT */
/* esp-swd target module: Nordic nRF5340 (application and network core).
 *
 * One SW-DP, four APs: AP0 application core AHB-AP, AP1 network core AHB-AP, AP2/AP3 their CTRL-APs. A session
 * attaches one core (esw_target_detect_core: 0 application, 1 network); flashing both cores is two attaches. */
#ifndef ESW_TARGETS_NRF53_H
#define ESW_TARGETS_NRF53_H
#include "esw/target.h"

#define ESW_NRF53_DPIDR        0x6BA02477u
#define ESW_NRF53_CTRL_AP_IDR  0x12880000u
#define ESW_NRF53_CORE_APP     0u
#define ESW_NRF53_CORE_NET     1u
#define ESW_NRF53_CTRL_AP(core) (2u + (core))

/* CTRL-AP registers (same first four as the nRF52's) */
#define ESW_NRF53_CTRLAP_RESET           0x000u
#define ESW_NRF53_CTRLAP_ERASEALL        0x004u
#define ESW_NRF53_CTRLAP_ERASEALLSTATUS  0x008u
#define ESW_NRF53_CTRLAP_APPROTECTSTATUS 0x00Cu   /* bit 0 APPROTECT disabled, bit 1 SECUREAPPROTECT disabled */

/* application core (secure aliases: the debugger's MEM-AP accesses are secure) */
#define ESW_NRF53_APP_FICR     0x00FF0000u
#define ESW_NRF53_APP_UICR     0x00FF8000u
#define ESW_NRF53_APP_NVMC     0x50039000u
#define ESW_NRF53_APP_RAM      0x20000000u
#define ESW_NRF53_NET_FORCEOFF 0x50005614u   /* RESET.NETWORK.FORCEOFF: 1 holds the network core off */
/* network core */
#define ESW_NRF53_NET_FICR     0x01FF0000u
#define ESW_NRF53_NET_UICR     0x01FF8000u
#define ESW_NRF53_NET_NVMC     0x41080000u
#define ESW_NRF53_NET_FLASH    0x01000000u
#define ESW_NRF53_NET_RAM      0x21000000u

#define ESW_NRF53_UICR_APPROTECT       0x000u   /* offsets in UICR */
#define ESW_NRF53_UICR_SECUREAPPROTECT 0x01Cu
#define ESW_NRF53_APPROTECT_UNPROTECTED 0x50FA50FAu

extern const esw_target_driver_t esw_target_nrf53;

/* Module helpers. */
esw_err_t esw_nrf53_erase_all(esw_target_t *t, uint32_t timeout_ms);   /* DESTRUCTIVE: this core's CTRL-AP ERASEALL */
esw_err_t esw_nrf53_ctrlap_reset(esw_target_t *t);

#endif
