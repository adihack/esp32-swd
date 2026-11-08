/* SPDX-License-Identifier: MIT */
/* esp-swd: error codes shared by every layer. */
#ifndef ESW_ERR_H
#define ESW_ERR_H

typedef enum {
    ESW_OK = 0,
    ESW_ERR_ARG = -1,          /* invalid argument */
    ESW_ERR_NO_ACK = -2,       /* no target response on SWDIO (ACK 0b111 / 0b000) */
    ESW_ERR_WAIT = -3,         /* WAIT retries exhausted */
    ESW_ERR_FAULT = -4,        /* FAULT ACK or sticky error in the DP (bus error, overrun, write parity) */
    ESW_ERR_PARITY = -5,       /* read-data parity mismatch */
    ESW_ERR_PROTOCOL = -6,     /* invalid ACK pattern */
    ESW_ERR_TIMEOUT = -7,
    ESW_ERR_LOCKED = -8,       /* debug access blocked by the target (APPROTECT, RDP, ...) */
    ESW_ERR_VERIFY = -9,
    ESW_ERR_STATE = -10,       /* call not valid in the current state */
    ESW_ERR_UNSUPPORTED = -11,
    ESW_ERR_NOT_FOUND = -12,   /* no target module matched, address outside every region, ... */
    ESW_ERR_LOADER = -13,      /* loader reported an error status */
    ESW_ERR_INTERLOCK = -14,   /* isolation interlock refused (VTref missing or out of range) */
} esw_err_t;

const char *esw_err_str(esw_err_t e);

#define ESW_TRY(expr) do { esw_err_t esw_try_e_ = (expr); if (esw_try_e_ != ESW_OK) return esw_try_e_; } while (0)

#endif
