/* SPDX-License-Identifier: MIT */
/* esp-swd: ARM ADIv5 debug port / access port layer over an SWD PHY, plus MEM-AP memory access. */
#ifndef ESW_ADIV5_H
#define ESW_ADIV5_H
#include <stdint.h>
#include "esw/err.h"
#include "esw/swd_phy.h"

/* DP registers (byte offsets; SELECT.DPBANKSEL = 0) */
#define ESW_DP_DPIDR      0x0u   /* R */
#define ESW_DP_ABORT      0x0u   /* W */
#define ESW_DP_CTRL_STAT  0x4u
#define ESW_DP_SELECT     0x8u   /* W */
#define ESW_DP_RDBUFF     0xCu   /* R */

#define ESW_ABORT_DAPABORT   0x01u
#define ESW_ABORT_CLEAR_ALL  0x1Eu   /* STKCMPCLR | STKERRCLR | WDERRCLR | ORUNERRCLR */

#define ESW_CS_ORUNDETECT    (1u << 0)
#define ESW_CS_STICKYORUN    (1u << 1)
#define ESW_CS_STICKYCMP     (1u << 4)
#define ESW_CS_STICKYERR     (1u << 5)
#define ESW_CS_WDATAERR      (1u << 7)
#define ESW_CS_CDBGPWRUPREQ  (1u << 28)
#define ESW_CS_CDBGPWRUPACK  (1u << 29)
#define ESW_CS_CSYSPWRUPREQ  (1u << 30)
#define ESW_CS_CSYSPWRUPACK  (1u << 31)
#define ESW_CS_STICKY_MASK   (ESW_CS_STICKYORUN | ESW_CS_STICKYCMP | ESW_CS_STICKYERR | ESW_CS_WDATAERR)

/* MEM-AP registers */
#define ESW_AP_CSW  0x00u
#define ESW_AP_TAR  0x04u
#define ESW_AP_DRW  0x0Cu
#define ESW_AP_IDR  0xFCu

#define ESW_CSW_SIZE32          0x00000002u
#define ESW_CSW_ADDRINC_SINGLE  0x00000010u
#define ESW_CSW_DEVICEEN        0x00000040u
#define ESW_CSW_CORTEXM_PROT    0x23000000u   /* privileged data access, debug master: Cortex-M AHB-AP default */

typedef struct {
    uint32_t xfers, wait, fault, noack, parity;
} esw_dap_stats_t;

typedef struct {
    esw_swd_phy_t phy;
    uint32_t dpidr;
    uint32_t select;
    uint8_t select_valid;
    uint8_t check_sticky;     /* 1 (default): single MEM-AP accesses sync on RDBUFF and check CTRL/STAT */
    uint16_t wait_retries;    /* WAIT retries per packet (default 100) */
    uint32_t last_ack;
    esw_dap_stats_t stats;
} esw_dap_t;

void esw_dap_init(esw_dap_t *d, esw_swd_phy_t phy);

/* Line reset, JTAG-to-SWD, line reset, idle, DPIDR read (leaves the DP out of its lock-out state). */
esw_err_t esw_dap_connect(esw_dap_t *d, uint32_t *dpidr);
esw_err_t esw_dap_power_up(esw_dap_t *d, uint32_t max_polls);
esw_err_t esw_dap_power_down(esw_dap_t *d);
esw_err_t esw_dap_clear_errors(esw_dap_t *d);
esw_err_t esw_dap_abort(esw_dap_t *d);               /* DAPABORT: cancels a transaction stuck in WAIT */
/* Sync on outstanding AP transactions (RDBUFF) and check CTRL/STAT; clears and reports sticky errors. */
esw_err_t esw_dap_check_errors(esw_dap_t *d);
void esw_dap_release(esw_dap_t *d);                  /* PHY to the tool-side safe state */

esw_err_t esw_dp_read(esw_dap_t *d, uint8_t reg, uint32_t *val);
esw_err_t esw_dp_write(esw_dap_t *d, uint8_t reg, uint32_t val);
esw_err_t esw_ap_read(esw_dap_t *d, uint8_t ap, uint8_t reg, uint32_t *val);
esw_err_t esw_ap_write(esw_dap_t *d, uint8_t ap, uint8_t reg, uint32_t val);

/* ---- MEM-AP ---------------------------------------------------------------------------------------- */
typedef struct {
    esw_dap_t *dap;
    uint8_t ap;
    uint32_t csw_base;   /* CSW bits other than Size and AddrInc (e.g. ESW_CSW_CORTEXM_PROT, HNONSEC) */
    uint32_t csw;
    uint32_t tar;
    uint8_t csw_valid, tar_valid;
} esw_memap_t;

void esw_memap_init(esw_memap_t *m, esw_dap_t *d, uint8_t ap, uint32_t csw_base);
void esw_memap_invalidate(esw_memap_t *m);           /* forget cached CSW/TAR (after a reset or reconnect) */
esw_err_t esw_mem_read32(esw_memap_t *m, uint32_t addr, uint32_t *val);
esw_err_t esw_mem_write32(esw_memap_t *m, uint32_t addr, uint32_t val);
/* Auto-increment block transfers; TAR is rewritten at every 1 KB boundary. One sticky check at the end. */
esw_err_t esw_mem_read_block(esw_memap_t *m, uint32_t addr, uint32_t *buf, uint32_t nwords);
esw_err_t esw_mem_write_block(esw_memap_t *m, uint32_t addr, const uint32_t *buf, uint32_t nwords);
/* Poll until (*addr & mask) == value; max_polls bounds the loop. */
esw_err_t esw_mem_poll32(esw_memap_t *m, uint32_t addr, uint32_t mask, uint32_t value, uint32_t max_polls,
                         uint32_t *last);

#endif
