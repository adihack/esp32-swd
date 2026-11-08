/* SPDX-License-Identifier: MIT */
/* esp-swd target module: Nordic nRF52 series (nRF52840 tested on hardware). */
#ifndef ESW_TARGETS_NRF52_H
#define ESW_TARGETS_NRF52_H
#include "esw/target.h"

#define ESW_NRF52_DPIDR        0x2BA01477u
#define ESW_NRF52_AHB_AP       0u
#define ESW_NRF52_CTRL_AP      1u
#define ESW_NRF52_CTRL_AP_IDR  0x02880000u

/* CTRL-AP registers */
#define ESW_NRF_CTRLAP_RESET           0x000u
#define ESW_NRF_CTRLAP_ERASEALL        0x004u
#define ESW_NRF_CTRLAP_ERASEALLSTATUS  0x008u
#define ESW_NRF_CTRLAP_APPROTECTSTATUS 0x00Cu

/* NVMC and UICR */
#define ESW_NRF_NVMC_READY      0x4001E400u
#define ESW_NRF_NVMC_CONFIG     0x4001E504u
#define ESW_NRF_NVMC_ERASEPAGE  0x4001E508u
#define ESW_NRF_UICR_PSELRESET0 0x10001200u
#define ESW_NRF_UICR_PSELRESET1 0x10001204u
#define ESW_NRF_UICR_APPROTECT  0x10001208u
#define ESW_NRF_APPROTECT_HWDISABLED 0xFFFFFF5Au

typedef struct {
    uint32_t part, variant, package, ram_kb, flash_kb, page_size, page_count;
    uint32_t deviceid[2];
} esw_nrf52_info_t;

extern const esw_target_driver_t esw_target_nrf52;

/* Module helpers, usable directly (e.g. by a recovery command). */
esw_err_t esw_nrf52_approtect_open(esw_target_t *t, uint32_t *open);
esw_err_t esw_nrf52_erase_all(esw_target_t *t, uint32_t timeout_ms);   /* DESTRUCTIVE; no reset afterwards */
esw_err_t esw_nrf52_ctrlap_reset(esw_target_t *t);
esw_err_t esw_nrf52_read_info(esw_target_t *t, esw_nrf52_info_t *info);
esw_err_t esw_nrf52_nvmc_write(esw_target_t *t, uint32_t addr, const uint32_t *words, uint32_t nwords);

#endif
